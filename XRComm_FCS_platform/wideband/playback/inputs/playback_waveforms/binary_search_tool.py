import sys

def get_hex_input(prompt):
    while True:
        val_str = input(prompt).strip()
        if val_str.lower() in ['exit', 'quit']:
            sys.exit(0)
        if not val_str:
            continue
        try:
            # Handle both "0x123" and "123"
            if val_str.lower().startswith("0x"):
                return int(val_str, 16)
            else:
                return int(val_str, 16)
        except ValueError:
            print("Invalid hex input. Please enter a valid hex number (e.g., 1A or 0x1A)")

def main():
    print("=== Binary Search Helper Tool ===")
    print("Commands:")
    print("  'h' or 'high' -> Value should be HIGHER than the current guess")
    print("  'l' or 'low'  -> Value should be LOWER than the current guess")
    print("  'r' or 'reset'-> Reset ranges")
    print("  'exit'        -> Quit")
    
    while True:
        print("\n--- New Search Range ---")
        low = get_hex_input("Enter Lower Bound (Hex): ")
        high = get_hex_input("Enter Upper Bound (Hex): ")
        
        if low > high:
            print("Swapping bounds (Low > High)...")
            low, high = high, low
            
        while True:
            # Calculate midpoint
            mid = (low + high) // 2
            
            print(f"\nCurrent Guess: 0x{mid:X}  (Unsigned: {mid})")
            print(f"Range: [0x{low:X} - 0x{high:X}]")
            
            if low >= high:
                print("Converged! The value should be approx: 0x{:X} ({})".format(low, low))
                break
            
            cmd = input("Go Higher or Lower? (h/l/r): ").strip().lower()
            
            if cmd in ['r', 'reset']:
                break # Break inner loop to restart inputs
            
            elif cmd in ['h', 'high', '+']:
                # The target is HIGHER than mid
                low = mid + 1
                
            elif cmd in ['l', 'low', '-']:
                # The target is LOWER than mid
                high = mid - 1
                
            elif cmd in ['exit', 'quit']:
                sys.exit(0)
            else:
                print("Unknown command. Use 'h' (Higher), 'l' (Lower), or 'r' (Reset)")

if __name__ == "__main__":
    main()
