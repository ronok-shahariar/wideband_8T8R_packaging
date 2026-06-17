// ============================================================================
//  xrcomm_playback_control.cc  —  XRComm FCS wideband platform
//
//  gRPC server for the PLAYBACK control surface (Guide Sections 8.3 & 8.4).
//
//  Design:
//    - The playback EXECUTABLE reads ONLY inputs/config.ini. It receives no
//      gRPC calls, no CLI args, no console answers.
//    - THIS SERVER is the only writer of inputs/config.ini. Each per-field
//      Set RPC validates its single value and rewrites ONLY its own key.
//    - StartPlayback launches xrcomm_playback via sudo (DPDK requires root)
//      with the working directory set to the playback tree root.
//      Child stdout+stderr are redirected to playback.log in the tree root
//      so crashes are always visible regardless of how the server was started.
//    - Diagnostics are read from playback_status.ini published by stats.c.
//
//  Environment variable:
//    XRCOMM_PLAYBACK_ROOT  — absolute path to the playback tree root.
//                            Defaults to the server's own working directory.
//    XRCOMM_SUDO           — set to "0" to skip the sudo wrapper (use when
//                            the server itself is already running as root).
// ============================================================================

#include <grpcpp/grpcpp.h>
#include "pipeline_control.grpc.pb.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <fstream>
#include <sstream>
#include <thread>
#include <chrono>

#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>

using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::ServerWriter;
using grpc::Status;
using grpc::StatusCode;

using namespace xrcomm;

// ---------------------------------------------------------------------------
//  Path helpers
// ---------------------------------------------------------------------------
static std::string tree_root() {
    const char *r = getenv("XRCOMM_PLAYBACK_ROOT");
    if (r && *r) return std::string(r);
    // fall back to cwd of the server process itself
    char buf[4096];
    if (getcwd(buf, sizeof(buf))) return std::string(buf);
    return std::string(".");
}
static std::string config_path()   { return tree_root() + "/inputs/config.ini"; }
static std::string waveform_dir()  { return tree_root() + "/inputs/playback_waveforms/"; }
static std::string playback_bin()  { return tree_root() + "/build/xrcomm_playback"; }
static std::string status_path()   { return tree_root() + "/playback_status.ini"; }
static std::string log_path()      { return tree_root() + "/playback.log"; }

// ---------------------------------------------------------------------------
//  Small string helpers
// ---------------------------------------------------------------------------
static void trim(std::string &s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    if (a == std::string::npos) { s.clear(); return; }
    s = s.substr(a, b - a + 1);
}

static std::string line_key(const std::string &line) {
    size_t hash = line.find('#');
    size_t eq   = line.find('=');
    if (eq == std::string::npos) return "";
    if (hash != std::string::npos && eq > hash) return "";
    std::string k = line.substr(0, eq);
    trim(k);
    return k;
}

// ---------------------------------------------------------------------------
//  Config defaults (Section 6.2)
// ---------------------------------------------------------------------------
static const std::map<std::string, std::string> kDefaults = {
    {"core_list",             "7,8,9"},
    {"source_mode",           "1"},
    {"binary_filename",       "tone_512.bin"},
    {"target_sample_rate_hz", "800000000"},
    {"tx_port_id",            "0"},
    {"dest_mac",              "14:FE:B5:DD:9A:82"},
    {"loop_count",            "0"},
    {"trigger_burst_size",    "1"},
};

// ---------------------------------------------------------------------------
//  Service
// ---------------------------------------------------------------------------
class PlaybackControlServiceImpl final : public PipelineControl::Service {
    std::mutex cfg_mtx_;
    std::mutex proc_mtx_;
    pid_t      child_     = -1;
    int        last_exit_ = 0;      // exit status of most recently reaped child

    // ---- read all current key/value pairs from config.ini ----
    std::map<std::string, std::string> read_config() {
        std::map<std::string, std::string> kv = kDefaults;
        std::ifstream in(config_path());
        if (!in) return kv;
        std::string line;
        while (std::getline(in, line)) {
            std::string k = line_key(line);
            if (k.empty()) continue;
            size_t eq = line.find('=');
            std::string v = line.substr(eq + 1);
            size_t hash = v.find('#');
            if (hash != std::string::npos) v = v.substr(0, hash);
            trim(v);
            kv[k] = v;
        }
        return kv;
    }

    std::string get_value(const std::string &key) {
        std::lock_guard<std::mutex> lock(cfg_mtx_);
        return read_config()[key];
    }

    // ---- rewrite exactly one key, leave all other lines byte-for-byte ----
    bool write_single_key(const std::string &key, const std::string &value) {
        std::lock_guard<std::mutex> lock(cfg_mtx_);
        std::vector<std::string> lines;
        bool found = false;
        {
            std::ifstream in(config_path());
            std::string line;
            while (in && std::getline(in, line)) lines.push_back(line);
        }
        for (auto &l : lines) {
            if (line_key(l) != key) continue;
            // preserve any inline comment that was already there
            std::string comment;
            size_t hash = l.find('#');
            if (hash != std::string::npos) comment = "   " + l.substr(hash);
            l = key + " = " + value + comment;
            found = true;
            break;
        }
        if (!found) lines.push_back(key + " = " + value);

        std::string tmp = config_path() + ".tmp";
        {
            std::ofstream out(tmp, std::ios::trunc);
            if (!out) return false;
            for (const auto &l : lines) out << l << "\n";
        }
        return std::rename(tmp.c_str(), config_path().c_str()) == 0;
    }

    void fill_snapshot(PlaybackConfig *out) {
        auto kv = read_config();
        out->set_core_list(kv["core_list"]);
        out->set_source_mode((uint32_t)strtoul(kv["source_mode"].c_str(), nullptr, 10));
        out->set_binary_filename(kv["binary_filename"]);
        out->set_target_sample_rate_hz(strtoull(kv["target_sample_rate_hz"].c_str(), nullptr, 10));
        out->set_tx_port_id((uint32_t)strtoul(kv["tx_port_id"].c_str(), nullptr, 10));
        out->set_dest_mac(kv["dest_mac"]);
        out->set_loop_count((uint32_t)strtoul(kv["loop_count"].c_str(), nullptr, 10));
        out->set_trigger_burst_size((uint32_t)strtoul(kv["trigger_burst_size"].c_str(), nullptr, 10));
    }

    Status snapshot_locked(PlaybackConfig *out) {
        std::lock_guard<std::mutex> lock(cfg_mtx_);
        fill_snapshot(out);
        return Status::OK;
    }

    void note_if_running(ServerContext *ctx) {
        std::lock_guard<std::mutex> lock(proc_mtx_);
        if (is_running_unlocked())
            ctx->AddTrailingMetadata("note",
                "playback is running; new value takes effect at next StartPlayback");
    }

    // ---- validation ----
    static bool valid_core_list(const std::string &v, std::string &err) {
        long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
        std::stringstream ss(v);
        std::string tok;
        bool any = false;
        while (std::getline(ss, tok, ',')) {
            trim(tok);
            if (tok.empty()) { err = "empty core entry"; return false; }
            for (char c : tok) if (!isdigit((unsigned char)c)) {
                err = "core '" + tok + "' is not an integer"; return false;
            }
            long c = strtol(tok.c_str(), nullptr, 10);
            if (ncpu > 0 && (c < 0 || c >= ncpu)) {
                err = "core " + tok + " not valid on host (0.." +
                      std::to_string(ncpu - 1) + ")";
                return false;
            }
            any = true;
        }
        if (!any) { err = "no cores given"; return false; }
        return true;
    }

    static bool valid_dest_mac(const std::string &v) {
        unsigned x[6]; char extra;
        return sscanf(v.c_str(), "%2x:%2x:%2x:%2x:%2x:%2x%c",
                      &x[0],&x[1],&x[2],&x[3],&x[4],&x[5],&extra) == 6;
    }

public:
    // -------------------- snapshot --------------------
    Status GetPlaybackConfig(ServerContext *, const Empty *, PlaybackConfig *r) override {
        return snapshot_locked(r);
    }

    // -------------------- getters --------------------
    Status GetCoreList(ServerContext *, const Empty *, CoreListValue *r) override {
        r->set_value(get_value("core_list")); return Status::OK;
    }
    Status GetSourceMode(ServerContext *, const Empty *, SourceModeValue *r) override {
        r->set_value((uint32_t)strtoul(get_value("source_mode").c_str(),nullptr,10));
        return Status::OK;
    }
    Status GetBinaryFilename(ServerContext *, const Empty *, BinaryFilenameValue *r) override {
        r->set_value(get_value("binary_filename")); return Status::OK;
    }
    Status GetSampleRate(ServerContext *, const Empty *, SampleRateValue *r) override {
        r->set_value(strtoull(get_value("target_sample_rate_hz").c_str(),nullptr,10));
        return Status::OK;
    }
    Status GetTxPortId(ServerContext *, const Empty *, TxPortIdValue *r) override {
        r->set_value((uint32_t)strtoul(get_value("tx_port_id").c_str(),nullptr,10));
        return Status::OK;
    }
    Status GetDestMac(ServerContext *, const Empty *, DestMacValue *r) override {
        r->set_value(get_value("dest_mac")); return Status::OK;
    }
    Status GetLoopCount(ServerContext *, const Empty *, LoopCountValue *r) override {
        r->set_value((uint32_t)strtoul(get_value("loop_count").c_str(),nullptr,10));
        return Status::OK;
    }
    Status GetTriggerBurstSize(ServerContext *, const Empty *, TriggerBurstSizeValue *r) override {
        r->set_value((uint32_t)strtoul(get_value("trigger_burst_size").c_str(),nullptr,10));
        return Status::OK;
    }

    // -------------------- setters --------------------
    Status SetCoreList(ServerContext *ctx, const CoreListValue *req, PlaybackConfig *resp) override {
        std::string err;
        if (!valid_core_list(req->value(), err))
            return Status(StatusCode::INVALID_ARGUMENT, "core_list: " + err);
        if (!write_single_key("core_list", req->value()))
            return Status(StatusCode::INTERNAL, "core_list: failed to write config.ini");
        note_if_running(ctx); return snapshot_locked(resp);
    }
    Status SetSourceMode(ServerContext *ctx, const SourceModeValue *req, PlaybackConfig *resp) override {
        if (req->value() != 0 && req->value() != 1)
            return Status(StatusCode::INVALID_ARGUMENT,
                          "source_mode: must be 0 (NVMe) or 1 (file)");
        if (!write_single_key("source_mode", std::to_string(req->value())))
            return Status(StatusCode::INTERNAL, "source_mode: failed to write config.ini");
        note_if_running(ctx); return snapshot_locked(resp);
    }
    Status SetBinaryFilename(ServerContext *ctx, const BinaryFilenameValue *req, PlaybackConfig *resp) override {
        std::string path = waveform_dir() + req->value();
        if (access(path.c_str(), F_OK) != 0)
            return Status(StatusCode::INVALID_ARGUMENT,
                          "binary_filename: file not found at " + path);
        if (!write_single_key("binary_filename", req->value()))
            return Status(StatusCode::INTERNAL, "binary_filename: failed to write config.ini");
        note_if_running(ctx); return snapshot_locked(resp);
    }
    Status SetSampleRate(ServerContext *ctx, const SampleRateValue *req, PlaybackConfig *resp) override {
        if (req->value() == 0)
            return Status(StatusCode::INVALID_ARGUMENT, "target_sample_rate_hz: must be > 0");
        if (!write_single_key("target_sample_rate_hz", std::to_string(req->value())))
            return Status(StatusCode::INTERNAL, "target_sample_rate_hz: failed to write config.ini");
        note_if_running(ctx); return snapshot_locked(resp);
    }
    Status SetTxPortId(ServerContext *ctx, const TxPortIdValue *req, PlaybackConfig *resp) override {
        if (req->value() > 0xFFFF)
            return Status(StatusCode::INVALID_ARGUMENT, "tx_port_id: must be a valid port id");
        if (!write_single_key("tx_port_id", std::to_string(req->value())))
            return Status(StatusCode::INTERNAL, "tx_port_id: failed to write config.ini");
        note_if_running(ctx); return snapshot_locked(resp);
    }
    Status SetDestMac(ServerContext *ctx, const DestMacValue *req, PlaybackConfig *resp) override {
        if (!valid_dest_mac(req->value()))
            return Status(StatusCode::INVALID_ARGUMENT,
                          "dest_mac: must parse as XX:XX:XX:XX:XX:XX");
        if (!write_single_key("dest_mac", req->value()))
            return Status(StatusCode::INTERNAL, "dest_mac: failed to write config.ini");
        note_if_running(ctx); return snapshot_locked(resp);
    }
    Status SetLoopCount(ServerContext *ctx, const LoopCountValue *req, PlaybackConfig *resp) override {
        if (!write_single_key("loop_count", std::to_string(req->value())))
            return Status(StatusCode::INTERNAL, "loop_count: failed to write config.ini");
        note_if_running(ctx); return snapshot_locked(resp);
    }
    Status SetTriggerBurstSize(ServerContext *ctx, const TriggerBurstSizeValue *req, PlaybackConfig *resp) override {
        if (req->value() < 1)
            return Status(StatusCode::INVALID_ARGUMENT, "trigger_burst_size: must be >= 1");
        if (!write_single_key("trigger_burst_size", std::to_string(req->value())))
            return Status(StatusCode::INTERNAL, "trigger_burst_size: failed to write config.ini");
        note_if_running(ctx); return snapshot_locked(resp);
    }

    // -------------------- process control --------------------
    bool is_running_unlocked() {
        if (child_ <= 0) return false;
        int st = 0;
        pid_t r = waitpid(child_, &st, WNOHANG);
        if (r == 0) return true;   // still running
        if (r > 0)  last_exit_ = st;
        child_ = -1;
        return false;
    }

    Status StartPlayback(ServerContext *, const Empty *, StatusReply *resp) override {
        std::lock_guard<std::mutex> lock(proc_mtx_);
        if (is_running_unlocked())
            return Status(StatusCode::FAILED_PRECONDITION, "playback already running");

        // Determine whether to wrap in sudo.
        // If the gRPC server itself runs as root (uid 0), sudo is unnecessary
        // and would actually fail. Set XRCOMM_SUDO=0 to override either way.
        bool use_sudo = (geteuid() != 0);
        const char *sudo_env = getenv("XRCOMM_SUDO");
        if (sudo_env) use_sudo = (std::string(sudo_env) != "0");

        std::string bin  = playback_bin();
        std::string root = tree_root();
        std::string log  = log_path();

        pid_t pid = fork();
        if (pid < 0)
            return Status(StatusCode::INTERNAL,
                          "fork failed: " + std::string(strerror(errno)));

        if (pid == 0) {
            // ---- child ----
            // Redirect stdout + stderr to playback.log so crashes are always
            // visible (the gRPC server's own terminal may not show them).
            int fd = open(log.c_str(),
                          O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd >= 0) {
                dup2(fd, STDOUT_FILENO);
                dup2(fd, STDERR_FILENO);
                close(fd);
            }

            // Set working directory to the playback tree root so that the
            // relative paths inputs/config.ini and inputs/playback_waveforms/
            // resolve correctly inside xrcomm_playback.
            if (chdir(root.c_str()) != 0) {
                fprintf(stderr, "[gRPC/child] chdir to '%s' failed: %s\n",
                        root.c_str(), strerror(errno));
                _exit(127);
            }

            if (use_sudo) {
                execl("/usr/bin/sudo", "sudo", bin.c_str(), (char *)nullptr);
            } else {
                execl(bin.c_str(), "xrcomm_playback", (char *)nullptr);
            }

            fprintf(stderr, "[gRPC/child] exec failed: %s\n", strerror(errno));
            _exit(127);
        }

        // ---- parent ----
        child_ = pid;
        resp->set_ok(true);
        resp->set_message("playback started (pid " + std::to_string(pid) +
                          ") — log: " + log);
        return Status::OK;
    }

    Status StopPlayback(ServerContext *, const Empty *, StatusReply *resp) override {
        std::lock_guard<std::mutex> lock(proc_mtx_);
        if (!is_running_unlocked())
            return Status(StatusCode::FAILED_PRECONDITION, "playback not running");

        kill(child_, SIGINT);
        int st = 0;
        waitpid(child_, &st, 0);
        last_exit_ = st;
        child_ = -1;
        resp->set_ok(true);
        resp->set_message("playback stopped");
        return Status::OK;
    }

    // -------------------- diagnostics --------------------
    void read_diagnostics(PlaybackDiagnostics *d) {
        bool running;
        int  last_exit;
        {
            std::lock_guard<std::mutex> lock(proc_mtx_);
            running   = is_running_unlocked();
            last_exit = last_exit_;
        }

        std::map<std::string, std::string> kv;
        std::ifstream in(status_path());
        std::string line;
        while (in && std::getline(in, line)) {
            size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string k = line.substr(0, eq), v = line.substr(eq + 1);
            trim(k); trim(v);
            kv[k] = v;
        }

        d->set_playback_running(running);

        if (!running) {
            // Surface the exit code so the operator can see crash reason
            // via the trigger_state field (string, not used when stopped).
            if (last_exit != 0) {
                if (WIFEXITED(last_exit))
                    d->set_trigger_state("EXITED:" + std::to_string(WEXITSTATUS(last_exit)));
                else if (WIFSIGNALED(last_exit))
                    d->set_trigger_state("SIGNAL:" + std::to_string(WTERMSIG(last_exit)));
            } else {
                d->set_trigger_state("STOPPED");
            }
            return;
        }

        auto dbl = [&](const char *k){ return kv.count(k) ? strtod(kv[k].c_str(), nullptr) : 0.0; };
        auto u64 = [&](const char *k){ return kv.count(k) ? strtoull(kv[k].c_str(), nullptr, 10) : 0ULL; };
        auto u32 = [&](const char *k){ return (uint32_t)(kv.count(k) ? strtoul(kv[k].c_str(), nullptr, 10) : 0); };
        auto i32 = [&](const char *k){ return (int32_t)(kv.count(k) ? strtol(kv[k].c_str(), nullptr, 10) : 0); };

        d->set_running_time_s(dbl("running_time_s"));
        d->set_loops_completed(u64("loops_completed"));
        d->set_loop_count_target(u32("loop_count_target"));
        d->set_trigger_state(kv.count("trigger_state") ? kv["trigger_state"] : "IDLE");
        d->set_trigger_burst_size(u32("trigger_burst_size"));
        d->set_total_triggered_pkts(u64("total_triggered_pkts"));
        d->set_tuning_rate_hz(u32("tuning_rate_hz"));
        d->set_tuning_offset(i32("tuning_offset"));
        d->set_read_input_gbps_instant(dbl("read_input_gbps_instant"));
        d->set_read_input_gbps_average(dbl("read_input_gbps_average"));
        d->set_wire_tx_gbps_instant(dbl("wire_tx_gbps_instant"));
        d->set_wire_tx_gbps_average(dbl("wire_tx_gbps_average"));
    }

    Status GetPlaybackDiagnostics(ServerContext *, const Empty *,
                                  PlaybackDiagnostics *resp) override {
        read_diagnostics(resp);
        return Status::OK;
    }

    Status WatchPlaybackDiagnostics(ServerContext *ctx, const Empty *,
                                    ServerWriter<PlaybackDiagnostics> *writer) override {
        while (!ctx->IsCancelled()) {
            PlaybackDiagnostics d;
            read_diagnostics(&d);
            if (!writer->Write(d)) break;
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        return Status::OK;
    }
};

int main(int argc, char **argv) {
    std::string addr = (argc > 1) ? argv[1] : "0.0.0.0:50051";

    printf("[gRPC] XRComm wideband playback controller\n");
    printf("[gRPC] playback tree root : %s\n", tree_root().c_str());
    printf("[gRPC] config.ini         : %s\n", config_path().c_str());
    printf("[gRPC] playback binary    : %s\n", playback_bin().c_str());
    printf("[gRPC] log file           : %s\n", log_path().c_str());
    printf("[gRPC] running as uid     : %d (sudo wrapper: %s)\n",
           geteuid(), (geteuid() != 0) ? "yes" : "no — already root");
    printf("[gRPC] listening on       : %s\n", addr.c_str());

    PlaybackControlServiceImpl service;
    ServerBuilder builder;
    builder.AddListeningPort(addr, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);

    std::unique_ptr<Server> server(builder.BuildAndStart());
    server->Wait();
    return 0;
}