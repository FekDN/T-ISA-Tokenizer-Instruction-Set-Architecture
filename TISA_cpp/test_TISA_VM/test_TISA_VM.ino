// Copyright (c) 2025-2026 Dmitry Feklin (FeklinDN@gmail.com)
// TISA VM for ESP32

#include <Arduino.h>
#include "TISA_VM.h"
#include "CYD28_SD.h"
#include <vector>
#include <string>

// SD card pins (standard for CYD28)
#define SD_SCK  18
#define SD_MISO 19
#define SD_MOSI 23
#define SD_CS   5

// Mutex for SD card (if used in multithreaded mode)
SemaphoreHandle_t g_sd_card_mutex = NULL;

// ============================================================================
// STRUCTURES FOR TESTING
// ============================================================================

struct TestCase {
    std::string model_id;
    std::string text;
    std::vector<uint8_t> manifest;
    std::vector<int32_t> ref_ids;
};

struct TestResult {
    bool passed;
    std::string model_id;
    std::string text;
    std::vector<int32_t> vm_ids;
    std::vector<int32_t> ref_ids;
    std::string error_msg;
};

struct TestStats {
    uint32_t total = 0;
    uint32_t passed = 0;
    uint32_t failed = 0;
    uint32_t errors = 0;
};

// ============================================================================
// MODEL RESOURCE LOADER
// ============================================================================

class ModelResourceLoader {
public:
static bool load_resources(const std::string& model_hash, VM_Resources& resources) {
    std::string base_path = "/models/" + model_hash + "/";
    
    Serial.printf("Loading model resources from: %s\n", base_path.c_str());
    
    // ================= vocab.b =================
    std::string vocab_path = base_path + "vocab.b";
    if (!sdcard.openFile(vocab_path.c_str())) {
        Serial.printf("ERROR: Cannot open vocab.b\n");
        return false;
    }
    
    uint32_t vocab_size;
    sdcard.readData((uint8_t*)&vocab_size, sizeof(vocab_size));
    
    size_t file_size = sdcard.size();
    uint64_t vocab_offset = sdcard.getPosition();
    uint32_t data_section_offset = vocab_offset + (vocab_size * sizeof(uint32_t));
    uint32_t vocab_data_size = file_size - data_section_offset;
    
    sdcard.closeFile();
    
    resources.vocab = std::make_unique<BinaryVocabView>(vocab_path, vocab_offset, vocab_data_size);
    
    Serial.printf("  ✓ vocab.b loaded: %u entries\n", resources.vocab->get_entry_count());
    
    // ================= vocab_idx.b =================
    std::string vocab_idx_path = base_path + "vocab_idx.b";
    if (!sdcard.openFile(vocab_idx_path.c_str())) {
        Serial.printf("ERROR: Cannot open vocab_idx.b\n");
        return false;
    }
    
    uint32_t idx_size;
    sdcard.readData((uint8_t*)&idx_size, sizeof(idx_size));
    
    file_size = sdcard.size();
    uint64_t idx_offset = sdcard.getPosition();
    uint32_t idx_data_size = file_size - idx_offset;
    
    sdcard.closeFile();
    
    resources.vocab_idx_for_decode = std::make_unique<BinaryVocabIndexView>(vocab_idx_path, idx_offset, idx_data_size);
    
    Serial.printf("  ✓ vocab_idx.b loaded: %u entries\n", resources.vocab_idx_for_decode->get_entry_count());
    
    // ================= merges.b =================
    std::string merges_path = base_path + "merges.b";
    if (sdcard.openFile(merges_path.c_str())) {
        uint32_t merges_count;
        sdcard.readData((uint8_t*)&merges_count, sizeof(merges_count));
        
        file_size = sdcard.size();
        uint64_t merges_offset = sdcard.getPosition();
        uint32_t merges_data_size = file_size - merges_offset;
        
        sdcard.closeFile();
        
        resources.merges = std::make_unique<BinaryMergesView>(merges_path, merges_offset, merges_data_size);
        
        Serial.printf("  ✓ merges.b loaded: %u entries\n", resources.merges->get_entry_count());
    } else {
        Serial.printf("  ℹ merges.b not found (not a BPE model)\n");
    }
    
    // ================= byte_map =================
    // O(256), без std::find
    std::array<bool, 256> used{};
    
    auto add_utf8 = [](uint32_t cp) {
        std::string s;
        if (cp < 0x80) {
            s += (char)cp;
        } else if (cp < 0x800) {
            s += (char)(0xC0 | (cp >> 6));
            s += (char)(0x80 | (cp & 0x3F));
        } else {
            s += (char)(0xE0 | (cp >> 12));
            s += (char)(0x80 | ((cp >> 6) & 0x3F));
            s += (char)(0x80 | (cp & 0x3F));
        }
        return s;
    };
    
    // базовый набор
    for (int i = 33; i <= 126; i++) used[i] = true;
    for (int i = 161; i <= 172; i++) used[i] = true;
    for (int i = 174; i <= 255; i++) used[i] = true;
    
    for (int i = 0; i < 256; i++) {
        if (used[i]) {
            resources.byte_map[(uint8_t)i] = add_utf8(i);
        }
    }
    
    uint32_t extra = 0;
    for (int i = 0; i < 256; i++) {
        if (!used[i]) {
            resources.byte_map[(uint8_t)i] = add_utf8(256 + extra);
            extra++;
        }
    }
    
    Serial.printf("  ✓ byte_map created: %u entries\n", resources.byte_map.size());
    
    return true;
}
};

// ============================================================================
// TEST KIT LOADER
// ============================================================================

class TestSuiteLoader {
public:
static bool load_test_suite(const char* filename, std::vector<TestCase>& test_cases) {
    if (!sdcard.openFile(filename)) {
        Serial.printf("ERROR: Cannot open test suite file: %s\n", filename);
        return false;
    }
    
    char magic[4];
    sdcard.readData((uint8_t*)magic, 4);
    if (memcmp(magic, "TSTS", 4) != 0) {
        Serial.printf("ERROR: Invalid magic\n");
        sdcard.closeFile();
        return false;
    }
    
    uint32_t test_count;
    sdcard.readData((uint8_t*)&test_count, sizeof(test_count));
    
    test_cases.clear();
    test_cases.reserve(test_count);
    
    std::vector<char> buffer;
    
    for (uint32_t i = 0; i < test_count; i++) {
        TestCase tc;
        
        uint16_t len16;
        uint32_t len32;
        
        // model_id
        sdcard.readData((uint8_t*)&len16, 2);
        buffer.resize(len16 + 1);
        sdcard.readData((uint8_t*)buffer.data(), len16);
        buffer[len16] = 0;
        tc.model_id.assign(buffer.data());
        
        // text
        sdcard.readData((uint8_t*)&len32, 4);
        buffer.resize(len32 + 1);
        sdcard.readData((uint8_t*)buffer.data(), len32);
        buffer[len32] = 0;
        tc.text.assign(buffer.data());
        
        // manifest
        sdcard.readData((uint8_t*)&len32, 4);
        tc.manifest.resize(len32);
        sdcard.readData(tc.manifest.data(), len32);
        
        // ref_ids
        sdcard.readData((uint8_t*)&len32, 4);
        tc.ref_ids.resize(len32);
        sdcard.readData((uint8_t*)tc.ref_ids.data(), len32 * 4);
        
        test_cases.push_back(std::move(tc));
    }
    
    sdcard.closeFile();
    return true;
}
};

// ============================================================================
// TEST RUNNER
// ============================================================================

class TISATestRunner {
private:
    std::map<std::string, std::string> model_hash_cache;
    
std::string get_model_hash(const std::string& model_id) {
    auto it = model_hash_cache.find(model_id);
    if (it != model_hash_cache.end()) return it->second;
    
    if (!sdcard.openFile("/models/model_map.txt")) return "";
    
    char line[512];
    
    while (true) {
        int pos = 0;
        
        while (pos < 511) {
            uint8_t ch;
            if (sdcard.readData(&ch, 1) == 0) break;
            if (ch == '\n') break;
            line[pos++] = ch;
        }
        
        if (pos == 0 && sdcard.getPosition() >= sdcard.size()) break;
        
        line[pos] = 0;
        
        char* colon = strchr(line, ':');
        if (colon) {
            *colon = 0;
            char* hash = colon + 1;
            
            // trim
            char* end = hash + strlen(hash) - 1;
            while (end >= hash && isspace(*end)) *end-- = 0;
            
            model_hash_cache[line] = hash;
            
            if (model_id == line) {
                sdcard.closeFile();
                return hash;
            }
        }
        
        if (sdcard.getPosition() >= sdcard.size()) break;
    }
    
    sdcard.closeFile();
    return "";
}
    
public:
    std::vector<TestResult> run_tests(const std::vector<TestCase>& test_cases) {
        std::vector<TestResult> results;
        results.reserve(test_cases.size());
        
        TestStats stats;
        stats.total = test_cases.size();
        
        Serial.printf("\n");
        Serial.printf("╔════════════════════════════════════════════════════════════════╗\n");
        Serial.printf("║                    STARTING TEST EXECUTION                     ║\n");
        Serial.printf("╚════════════════════════════════════════════════════════════════╝\n");
        Serial.printf("\n");
        
        for (size_t i = 0; i < test_cases.size(); i++) {
            const TestCase& tc = test_cases[i];
            
            Serial.printf("\n");
            Serial.printf("──────────────────────────────────────────────────────────────────\n");
            Serial.printf("Test %u/%u: %s\n", i + 1, test_cases.size(), tc.model_id.c_str());
            Serial.printf("──────────────────────────────────────────────────────────────────\n");
            Serial.printf("Text: \"%s\"\n", tc.text.c_str());
            
            TestResult result;
            result.model_id = tc.model_id;
            result.text = tc.text;
            result.ref_ids = tc.ref_ids;
            
            // Get model hash
            std::string model_hash = get_model_hash(tc.model_id);
            if (model_hash.empty()) {
                result.passed = false;
                result.error_msg = "Model hash not found in model_map.txt";
                stats.errors++;
                Serial.printf("❌ ERROR: %s\n", result.error_msg.c_str());
                results.push_back(result);
                continue;
            }
            
            Serial.printf("Model hash: %s\n", model_hash.c_str());
            
            // Loading model resources
            VM_Resources resources;
            if (!ModelResourceLoader::load_resources(model_hash, resources)) {
                result.passed = false;
                result.error_msg = "Failed to load model resources";
                stats.errors++;
                Serial.printf("❌ ERROR: %s\n", result.error_msg.c_str());
                results.push_back(result);
                continue;
            }
            
            // Create a VM and run tokenization
            try {
                TISAVM vm(resources);
                
                Serial.printf("\nRunning TISA VM...\n");
                uint32_t start_time = millis();
                result.vm_ids = vm.run(tc.manifest, tc.text);
                uint32_t elapsed_time = millis() - start_time;
                
                Serial.printf("Execution time: %u ms\n", elapsed_time);
                Serial.printf("\nResults:\n");
                Serial.printf("  VM IDs:  [");
                for (size_t j = 0; j < result.vm_ids.size(); j++) {
                    Serial.printf("%d", result.vm_ids[j]);
                    if (j < result.vm_ids.size() - 1) Serial.printf(", ");
                }
                Serial.printf("]\n");
                
                Serial.printf("  Ref IDs: [");
                for (size_t j = 0; j < result.ref_ids.size(); j++) {
                    Serial.printf("%d", result.ref_ids[j]);
                    if (j < result.ref_ids.size() - 1) Serial.printf(", ");
                }
                Serial.printf("]\n");
                
                // Checking the result
                result.passed = (result.vm_ids == result.ref_ids);
                
                if (result.passed) {
                    stats.passed++;
                    Serial.printf("\n✅ PASS\n");
                } else {
                    stats.failed++;
                    Serial.printf("\n❌ FAIL: Token mismatch\n");
                    
                    // Detailed comparison
                    Serial.printf("\nDifferences:\n");
                    size_t max_len = std::max(result.vm_ids.size(), result.ref_ids.size());
                    for (size_t j = 0; j < max_len; j++) {
                        if (j < result.vm_ids.size() && j < result.ref_ids.size()) {
                            if (result.vm_ids[j] != result.ref_ids[j]) {
                                Serial.printf("  [%u] VM: %d, Ref: %d ❌\n", j, result.vm_ids[j], result.ref_ids[j]);
                            }
                        } else if (j >= result.vm_ids.size()) {
                            Serial.printf("  [%u] VM: <missing>, Ref: %d ❌\n", j, result.ref_ids[j]);
                        } else {
                            Serial.printf("  [%u] VM: %d, Ref: <missing> ❌\n", j, result.vm_ids[j]);
                        }
                    }
                }
                
            } catch (const std::exception& e) {
                result.passed = false;
                result.error_msg = std::string("Exception: ") + e.what();
                stats.errors++;
                Serial.printf("❌ ERROR: %s\n", result.error_msg.c_str());
            }
            
            results.push_back(result);
            
            //delay(50);
        }
        
        // Вывод итоговой статистики
        Serial.printf("\n\n");
        Serial.printf("╔════════════════════════════════════════════════════════════════╗\n");
        Serial.printf("║                        TEST SUMMARY                            ║\n");
        Serial.printf("╠════════════════════════════════════════════════════════════════╣\n");
        Serial.printf("║  Total tests:  %3u                                             ║\n", stats.total);
        Serial.printf("║  Passed:       %3u  (%.1f%%)                                   ║\n", 
                      stats.passed, (stats.total > 0 ? (100.0 * stats.passed / stats.total) : 0.0));
        Serial.printf("║  Failed:       %3u  (%.1f%%)                                   ║\n", 
                      stats.failed, (stats.total > 0 ? (100.0 * stats.failed / stats.total) : 0.0));
        Serial.printf("║  Errors:       %3u  (%.1f%%)                                   ║\n", 
                      stats.errors, (stats.total > 0 ? (100.0 * stats.errors / stats.total) : 0.0));
        Serial.printf("╚════════════════════════════════════════════════════════════════╝\n");
        
        return results;
    }
};

// ============================================================================
// MAIN FUNCTION FOR RUNNING TESTS
// ============================================================================

void run_tisa_test_suite() {
    Serial.printf("\n\n");
    Serial.printf("╔════════════════════════════════════════════════════════════════╗\n");
    Serial.printf("║              TISA VM TEST SUITE v1.0                           ║\n");
    Serial.printf("║              Copyright (c) 2025 Dmitry Feklin                  ║\n");
    Serial.printf("╚════════════════════════════════════════════════════════════════╝\n");
    Serial.printf("\n");
    
    // Loading the test set
    std::vector<TestCase> test_cases;
    if (!TestSuiteLoader::load_test_suite("/tisa_test_suite.bin", test_cases)) {
        Serial.printf("FATAL ERROR: Failed to load test suite!\n");
        return;
    }
    
    // Let's run tests
    TISATestRunner runner;
    auto results = runner.run_tests(test_cases);
    
    // Can save the results to an SD card for analysis.
    // save_test_results(results);
    
    Serial.printf("\n\nTest suite completed.\n");
}


void setup() {
    Serial.begin(115200);
    delay(2000);  // Give it time to open the Serial Monitor.
    
    Serial.println("\n\n");
    Serial.println("================================================");
    Serial.println("       TISA VM Test Suite Example");
    Serial.println("================================================");
    
    // Create a mutex for the SD card
    g_sd_card_mutex = xSemaphoreCreateMutex();
    if (g_sd_card_mutex == NULL) {
        Serial.println("ERROR: Failed to create SD mutex");
        return;
    }
    
    // Initializing the SD card
    Serial.println("\nInitializing SD card...");
    sdcard.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
    
    // Checking the SD card status
    uint8_t mount_status = 0;
    sdcard.status(&mount_status, NULL, NULL, NULL, NULL);
    
    if (!mount_status) {
        Serial.println("ERROR: SD card not mounted!");
        Serial.println("Please check:");
        Serial.println("  1. SD card is inserted");
        Serial.println("  2. SD card is formatted (FAT32)");
        Serial.println("  3. Pin connections are correct");
        return;
    }
    
    char status_buf[256];
    sdcard.printStatus(status_buf);
    Serial.println(status_buf);
    
    // Checking for the presence of a test file
    Serial.println("\nChecking for test suite file...");
    if (!sdcard.openFile("/tisa_test_suite.bin")) {
        Serial.println("ERROR: Test suite file not found!");
        Serial.println("Please ensure tisa_test_suite.bin is in the root of SD card");
        Serial.println("\nTo generate test suite:");
        Serial.println("  1. Run: python generate_test_suite.py");
        Serial.println("  2. Copy tisa_build/* to SD card root");
        return;
    }
    sdcard.closeFile();
    Serial.println("✓ Test suite file found");
    
    // Checking for the existence of the models folder
    Serial.println("\nChecking for models directory...");
    // Let's try opening model_map.txt
    if (!sdcard.openFile("/models/model_map.txt")) {
        Serial.println("ERROR: Models directory not found!");
        Serial.println("Please ensure models/ directory is in the root of SD card");
        return;
    }
    sdcard.closeFile();
    Serial.println("✓ Models directory found");
    
    Serial.println("\n================================================");
    Serial.println("       All checks passed! Starting tests...");
    Serial.println("================================================");
    
    delay(2000);
    
    // Running the tests
    run_tisa_test_suite();
    
    Serial.println("\n\n================================================");
    Serial.println("       Test suite execution completed");
    Serial.println("================================================");
}

void loop() {
    delay(10000);
    
    // Add an interactive menu:
    if (Serial.available()) {
        char cmd = Serial.read();
        if (cmd == 'r' || cmd == 'R') {
            Serial.println("\n\nRestarting test suite...\n");
            run_tisa_test_suite();
        } else if (cmd == 'h' || cmd == 'H') {
            Serial.println("\n=== HELP ===");
            Serial.println("Commands:");
            Serial.println("  r/R - Rerun test suite");
            Serial.println("  h/H - Show this help");
            Serial.println("============\n");
        }
    }
}
