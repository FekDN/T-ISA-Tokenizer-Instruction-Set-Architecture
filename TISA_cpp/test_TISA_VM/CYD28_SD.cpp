// CYD28_SD.cpp
// A lightweight version for reading NAC models

#include "CYD28_SD.h"

// Global objects 'extern' for them will be in the .h file
SPIClass sd_spi(VSPI);
CYD28_SD sdcard;

/**
 * @brief Initializes and mounts the SD card.
 */
void CYD28_SD::begin(int8_t sck, int8_t miso, int8_t mosi, int8_t cs)
{
	_cs_pin = cs;
	_is_initialized = false;

  // We do NOT call sd_spi.begin() here.
  // The VSPI bus is initialized by LovyanGFX (tft.init()).
  // We simply tell the SD library to use the existing configuration.
  // sd_spi.begin(sck, miso, mosi, cs); // <-- LINE REMOVED/COMMENTED

	if (!SD.begin(_cs_pin, sd_spi, 25000000))
	{
		Serial.println("Card Mount Failed");
		return;
	}
	uint8_t cardType = SD.cardType();

	if (cardType == CARD_NONE)
	{
		Serial.println("No SD card attached");
		return;
	}
    
    _is_initialized = true; 
    Serial.println("SD Card mounted successfully.");
}

/**
 * @brief Opens a file for reading and saves it in _current_file.
 * @param path Path to the file.
 * @return true if the file was successfully opened.
 */
bool CYD28_SD::openFile(const char* path) {
    if (!_is_initialized) {
        Serial.println("SD card not initialized. Cannot open file.");
        return false;
    }
    if (_current_file) {
        _current_file.close();
    }
    _current_file = SD.open(path, FILE_READ);
    if (!_current_file) {
        Serial.printf("Failed to open file for reading: %s\n", path);
        return false;
    }
    return true;
}

/**
 * @brief Closes the currently open file.
 */
void CYD28_SD::closeFile() {
    if (_current_file) {
        _current_file.close();
    }
}

/**
 * @brief Moves the read pointer within the open file.
 * @param position The offset from the beginning of the file.
 * @return true if the move was successful.
 */
bool CYD28_SD::seek(size_t position) {
    if (!_current_file) return false;
    return _current_file.seek(position);
}
bool CYD28_SD::seek(uint32_t pos, SeekMode mode) {
    if (!_current_file) return false;
    return _current_file.seek(pos, mode);
}
/**
 * @brief Reads data from the current position in the open file.
 * @param buffer Pointer to the buffer where the data is written.
 * @param length Number of bytes to read.
 * @return Number of bytes actually read.
 */
size_t CYD28_SD::readData(uint8_t* buffer, size_t length) {
    if (!_current_file) return 0;
    return _current_file.read(buffer, length);
}

/**
 * @brief Returns the current position of the file pointer.
 */
size_t CYD28_SD::getPosition() {
    if (!_current_file) return 0;
    return _current_file.position();
}

/**
 * @brief Checks if any file is currently open.
 */
bool CYD28_SD::isFileOpen() {
    return (bool)_current_file;
}

/**
 * @brief Returns the size of an open file.
 */
size_t CYD28_SD::size() {
    if (!_current_file) return 0;
    return _current_file.size();
}

/**
 * @brief Gets the status of the SD card as raw data.
 */
void CYD28_SD::status(uint8_t *mount, uint8_t *type, uint64_t *size, 
			        uint64_t *totalBytes, uint64_t *usedBytes)
{
	if (mount) *mount = _is_initialized;
	if (!_is_initialized) return;

	if (type)		*type = SD.cardType();
	if (size)		*size = SD.cardSize();
	if (totalBytes) *totalBytes = SD.totalBytes();
	if (usedBytes)	*usedBytes = SD.usedBytes();
}

/**
 * @brief Formats the SD card status into a text string.
 */
void CYD28_SD::printStatus(char *buf)
{
	if (buf == NULL) return;

	if (!_is_initialized)
	{
		snprintf(buf, 20, "SD CARD not found!");
	}
	else
	{
		uint8_t type = SD.cardType();
		uint64_t size = SD.cardSize() / 1048576;
		uint64_t totalBytes = SD.totalBytes() / 1048576;
		uint64_t usedBytes = SD.usedBytes() / 1048576;

		snprintf( buf, 255,
			"SD card found\n"
			"Type: %s\n"
			"Size: %llu MB\n"
			"Total: %llu MB\n"
			"Used: %llu MB\n", 
			sdcardTypeLabels[type], 
			size, 
			totalBytes, 
			usedBytes);
	}
}