import sys
import os

def split_binary_file(file_path):
    if not os.path.isfile(file_path):
        print(f"File not found: {file_path}")
        return

    # Prepare output filenames
    dir_name = os.path.dirname(file_path)
    base_name = os.path.basename(file_path)
    file_name, ext = os.path.splitext(base_name)
    
    # 64 bits = 8 bytes
    CHUNK_SIZE = 8
    
    # Create filenames like tone_512_0.bin, tone_512_1.bin, etc.
    out_paths = [os.path.join(dir_name, f"{file_name}_{i}{ext}") for i in range(4)]
    out_files = [open(path, 'wb') for path in out_paths]
    
    print(f"Splitting '{base_name}' into 4 files (Chunk: 64 bits / 8 bytes)...")
    
    total_bytes = 0
    try:
        with open(file_path, 'rb') as f:
            while True:
                # Read a block containing one chunk for each file (4 * 8 = 32 bytes)
                # This ensures we handle the round-robin cleanly 
                # (first chunk of block -> file 0, second -> file 1 etc)
                block = f.read(CHUNK_SIZE * 4)
                if not block:
                    break
                
                # Iterate through the chunks in this block
                for i in range(0, len(block), CHUNK_SIZE):
                     chunk = block[i : i+CHUNK_SIZE]
                     
                     # Determine destination file index (0, 1, 2, 3)
                     chunk_slot = i // CHUNK_SIZE
                     
                     # Write to the corresponding file
                     out_files[chunk_slot].write(chunk)
                     
                total_bytes += len(block)
                     
    except Exception as e:
        print(f"Error processing file: {e}")
    finally:
        for f in out_files:
            f.close()
            
    print(f"Done. Processed {total_bytes} bytes.")
    for p in out_paths:
        if os.path.exists(p):
            print(f"  - {os.path.basename(p)} ({os.path.getsize(p)} bytes)")

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: python3 split_bin.py <input_bin_file>")
        print("Example: python3 split_bin.py tone_512.bin")
    else:
        split_binary_file(sys.argv[1])
