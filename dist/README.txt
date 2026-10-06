CRUSH — from-scratch compressor (Windows x64 build)

  crush a out.crush file1 file2 ...   create / add
  crush x out.crush [dest]            extract
  crush l out.crush                   list + ratios
  crush t out.crush                   test integrity

Standalone: no runtime DLLs needed. Build from source: g++ -O2 -std=c++17 -static -o crush.exe crush.cpp
