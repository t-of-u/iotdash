#pragma once
// Program images in flash: the OS image (partition "sys") and app images (partition "slots",
// three 1.5 MB slots) are copied there from the card (or the flash file system) and read from
// there on demand (PartitionSource: through the flash driver, paced, never mapped). A slot
// remembers which image (key + content hash) it holds: launching the same app again uses it
// right away.
//
// Slot layout: a 4 KB header sector, then the image.

#include <esp_partition.h>

#include <memory>
#include <string>

#include "engine.h"

std::unique_ptr<rt::ImageLoad> loadImageToFlash(const std::string &key, const std::string &path, const std::string &hash);
