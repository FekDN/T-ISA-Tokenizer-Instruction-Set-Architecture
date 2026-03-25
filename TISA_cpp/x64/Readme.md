````
project/
├── tisa_test_x64.cpp
├── CMakeLists.txt
├── build_tisa_x64.bat
├── TISA_UCD_TABLES.h
└── (next to the exe after assembly):
    ├── tisa_test_suite.bin
    └── models/
        ├── model_map.txt    ← "model_id:hash" per line
        └── <hash>/
            ├── vocab.b
            ├── vocab_idx.b
            └── merges.b     (BPE only)
````
Benchmark with 1000 iterations for each model:
tisa_test_x64.exe --bench   
