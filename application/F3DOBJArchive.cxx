#include "F3DOBJArchive.h"

#include "log.h"

#include <regex>
#include <zip.h>

//----------------------------------------------------------------------------
F3DOBJArchive::F3DOBJArchive(const std::filesystem::path& archivePath)
{
  archiveName = archivePath.string();
  isValid = DecompressZipToBuffers();
  SortDecompressedData();
}

//----------------------------------------------------------------------------
F3DOBJArchive::~F3DOBJArchive()
{
  // Deallocate all buffers
  for (auto& buffer : decompressedData)
  {
    delete [] std::get<0>(buffer);
  }
}

//----------------------------------------------------------------------------
void F3DOBJArchive::SortDecompressedData()
{
  std::vector<size_t> modelIndexes;
  std::vector<size_t> materialIndexes;
  std::vector<size_t> textureIndexes;
  std::vector<size_t> otherIndexes;

  std::regex modelPattern(R"(\.(?:obj|ply|gltf))", std::regex::icase);
  std::regex materialPattern(R"(\.mtl)", std::regex::icase);
  std::regex texturePattern(R"(\.(?:png|jpg|jpeg))", std::regex::icase);

  std::smatch matches;
  for (size_t i = 0; i < decompressedData.size(); ++i)
  {
    const std::string& bufferFilename = std::get<2>(decompressedData[i]);

    // Check if the buffer filename matches regex pattern
    if (std::regex_search(bufferFilename, matches, modelPattern))
    {
      modelIndexes.push_back(i);
    }
    else if (std::regex_search(bufferFilename, matches, materialPattern))
    {
      materialIndexes.push_back(i);
    }
    else if (std::regex_search(bufferFilename, matches, texturePattern))
    {
      textureIndexes.push_back(i);
    }
    else
    {
      otherIndexes.push_back(i);
    }
  }

  // Re-order the data and compute ranges
  std::vector<f3d::scene::bufferTuple> reorderedData;
  reorderedData.reserve(decompressedData.size());

  for (size_t index: modelIndexes) { reorderedData.push_back(decompressedData[index]); }
  std::pair<size_t, size_t> modelsRange = { 0, modelIndexes.size() };

  for (size_t index: materialIndexes) { reorderedData.push_back(decompressedData[index]); }
  std::pair<size_t, size_t> materialsRange = { modelsRange.second, reorderedData.size() };

  for (size_t index: textureIndexes) { reorderedData.push_back(decompressedData[index]); }
  std::pair<size_t, size_t> texturesRange = { materialsRange.second, reorderedData.size() };

  for (size_t index: otherIndexes) { reorderedData.push_back(decompressedData[index]); }
  std::pair<size_t, size_t> otherRange = { texturesRange.second, reorderedData.size() };

  // Replace original vector with sorted vector
  decompressedData = reorderedData;

  // Create sub-ranges of data
  decompressedModels = std::vector(decompressedData.begin() + modelsRange.first,
    decompressedData.begin() + modelsRange.second);
  decompressedMaterials = std::vector(decompressedData.begin() + materialsRange.first,
    decompressedData.begin() + materialsRange.second);
  decompressedTextures = std::vector(decompressedData.begin() + texturesRange.first,
    decompressedData.begin() + texturesRange.second);
  decompressedOther = std::vector(decompressedData.begin() + otherRange.first,
    decompressedData.begin() + otherRange.second);
}

//----------------------------------------------------------------------------
bool F3DOBJArchive::DecompressZipToBuffers() {
  // Initialize variables
  int err = 0;
  zip* za;

  // Open the ZIP archive
  f3d::log::debug("F3DOBJArchive: Opening archive '", archiveName, "'");
  if ((za = zip_open(archiveName.c_str(), ZIP_RDONLY, &err)) == nullptr) {
    // Log error message
    zip_error_t error;
    zip_error_init_with_code(&error, err);
    f3d::log::warn("F3DOBJArchive: Failed to open zip archive '", archiveName, "': ", zip_error_strerror(&error));

    // Release resources and return failure
    zip_error_fini(&error);
    return false;
  }

  // Get number of files in archive
  f3d::log::debug("F3DOBJArchive: Checking archive contents");
  int64_t count = 0;
  count = zip_get_num_entries(za, ZIP_FL_UNCHANGED);

  // Loop over files and attempt to decompress each one
  struct zip_stat st;
  for (size_t i = 0; i < count; i++)
  {
    // Get info for the first file in the archive
    zip_stat_init(&st);
    zip_stat_index(za, i, ZIP_FL_UNCHANGED, &st);

    // Check that filename and data size are valid
    if (!(st.valid & ZIP_STAT_NAME) || !(st.valid & ZIP_STAT_SIZE)) {
      // Log error message
      f3d::log::warn("F3DOBJArchive: Error determining name or size of compressed file ", i, ", skipping.");
    }
    else
    {
      // Extract data to decompressed buffer
      f3d::scene::bufferTuple bufferData = DecompressIndexToBuffer(i, za, st);
      if (std::get<0>(bufferData) != nullptr && std::get<1>(bufferData) > 0)
      {
        decompressedData.push_back(bufferData);
      }
      else
      {
        f3d::log::warn("F3DOBJArchive: Archive file ", i, " returned null data or zero size.");
      }
    }
  }

  // Release resources
  f3d::log::debug("F3DOBJArchive: Releasing archive resources");
  zip_close(za);

  // Did we successfully extract at least one file?
  return decompressedData.size() > 0;
}

//----------------------------------------------------------------------------
f3d::scene::bufferTuple F3DOBJArchive::DecompressIndexToBuffer(const size_t index, zip* za, const struct zip_stat& st)
{
  // Save data to a tuple
  f3d::scene::bufferTuple buffer;
  std::get<2>(buffer) = std::string(st.name);

  // Open the file in the archive for reading
  f3d::log::debug("F3DOBJArchive: Opening file inside archive '", st.name, "'");
  zip_file* zf;
  if ((zf = zip_fopen_index(za, index, 0)) == nullptr) {
    // Log error message
    zip_error_t* error = zip_get_error(za);
    f3d::log::warn("F3DOBJArchive: Failed to open compressed file '", st.name, "': ", zip_error_strerror(error));
    return {nullptr, 0, ""};
  }

  // Alloc memory for the decompressed contents
  f3d::log::debug("F3DOBJArchive: Allocating ", st.size, " bytes for '", st.name, "'");
  std::get<1>(buffer) = st.size;
  const auto rawData = new std::byte[st.size];

  // Read all the data (also decompresses the data)
  f3d::log::debug("F3DOBJArchive: Reading '", st.name, "' from archive");
  if (zip_fread(zf, rawData, st.size) != st.size) {
    // Log error message
    zip_error_t* error = zip_get_error(za);
    f3d::log::warn("F3DOBJArchive: Failed to read file '", st.name, "' in archive - ", zip_error_strerror(error));

    // Cleanup and clear buffer
    delete [] rawData;
    std::get<0>(buffer) = nullptr;
    std::get<1>(buffer) = 0;
  }
  else
  {
    std::get<0>(buffer) = rawData;
  }

  // Release internal zipfile and return buffer
  zip_fclose(zf);
  return buffer;
}
