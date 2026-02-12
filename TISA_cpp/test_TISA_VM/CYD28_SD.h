// CYD28_SD.h
// A lightweight version for reading NAC models

#ifndef _CYD28_SD_H
#define _CYD28_SD_H

#include <Arduino.h>
#include "SD.h"
#include "SPI.h"

class CYD28_SD
{
public:
	CYD28_SD(){};
	~CYD28_SD(){};

	void begin(int8_t sck, int8_t miso, int8_t mosi, int8_t cs);
	
	// --- FUNCTIONS FOR WORKING WITH A MODEL FILE ---
    bool openFile(const char* path);
    void closeFile();
    bool seek(size_t position);
    bool seek(uint32_t pos, SeekMode mode);
    size_t readData(uint8_t* buffer, size_t length);
    size_t getPosition();
    bool isFileOpen();
	size_t size();
	// --- STATUS AND DEBUG FUNCTIONS ---
	void status(uint8_t *mount, uint8_t *type, uint64_t *size, 
				uint64_t *totalBytes, uint64_t *usedBytes);
	void printStatus(char *buf); 		
	
private:
    int8_t _cs_pin = -1;
    bool _is_initialized = false;
	const char *sdcardTypeLabels[5] = { "None", "MMC", "SD", "SDHC", "Unknown"};
    File _current_file; 
};

// 'extern' says that these objects are created somewhere else (in the .cpp file)
extern CYD28_SD sdcard;
extern SPIClass sd_spi;

#endif // _CYD28_SD_H