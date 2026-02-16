"""
PlatformIO pre-build script to compress web files for filesystem upload
Compresses files from data_src/ to data/*.gz before buildfs
"""
Import('env')
import os
import gzip
import shutil

def compress_web_files(source, target, env):
    """Compress web files from data_src/ to data/"""
    project_dir = env.get("PROJECT_DIR")
    data_src_dir = os.path.join(project_dir, "data_src")
    data_dir = os.path.join(project_dir, "data")
    
    # Create data dir if it doesn't exist
    os.makedirs(data_dir, exist_ok=True)
    
    # Clean old .gz files
    for file in os.listdir(data_dir):
        if file.endswith('.gz'):
            os.remove(os.path.join(data_dir, file))
    
    # Compress each file from data_src
    if not os.path.exists(data_src_dir):
        print("Warning: data_src/ directory not found")
        return
    
    total_uncompressed = 0
    total_compressed = 0
    
    for file in os.listdir(data_src_dir):
        src_path = os.path.join(data_src_dir, file)
        if not os.path.isfile(src_path):
            continue
            
        dst_path = os.path.join(data_dir, file + '.gz')
        
        # Read and compress
        with open(src_path, 'rb') as f_in:
            data = f_in.read()
            total_uncompressed += len(data)
            
            with gzip.open(dst_path, 'wb', compresslevel=9) as f_out:
                f_out.write(data)
                
        compressed_size = os.path.getsize(dst_path)
        total_compressed += compressed_size
        
        print(f"  Compressed: {file} ({len(data)} -> {compressed_size} bytes, "
              f"{100 - int(compressed_size * 100 / len(data))}% reduction)")
    
    if total_uncompressed > 0:
        print(f"Total: {total_uncompressed} -> {total_compressed} bytes "
              f"({100 - int(total_compressed * 100 / total_uncompressed)}% reduction)")

# Register the callback to run before buildfs
env.AddPreAction("$BUILD_DIR/littlefs.bin", compress_web_files)
env.AddPreAction("$BUILD_DIR/spiffs.bin", compress_web_files)

print("=== Web file compression script loaded ===")
