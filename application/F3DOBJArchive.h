/**
 * @class   F3DOBJArchive
 * @brief   A 3d model inside a zip archive.
 *
 * A class to hold and manage a 3d model stored inside of a zip archive. Can
 * support any archive supported by libzip.
 */

#ifndef F3DOBJArchive_h
#define F3DOBJArchive_h

#include <filesystem>
#include <scene.h>

struct zip;
struct zip_stat;

class F3DOBJArchive
{
public:
  explicit F3DOBJArchive(const std::filesystem::path& archivePath);
  ~F3DOBJArchive();

  // Access the isValid variable
  bool IsValid() const { return isValid; }

  // Access the archive name
  std::string ArchiveName() const { return archiveName; }

  // Access the decompressed data
  size_t DataCount() const { return decompressedData.size(); }
  const f3d::scene::bufferTuple& DecompressedData(const int index) const { return decompressedData[index]; }
  const std::vector<f3d::scene::bufferTuple>& DecompressedData() const { return decompressedData; }

  const std::vector<f3d::scene::bufferTuple>& DecompressedModelData() const { return decompressedModels; }
  const std::vector<f3d::scene::bufferTuple>& DecompressedTextureData() const { return decompressedTextures; }
  const std::vector<f3d::scene::bufferTuple>& DecompressedMaterialData() const { return decompressedMaterials; }
  const std::vector<f3d::scene::bufferTuple>& DecompressedOtherData() const { return decompressedOther; }

  // Delete default constructors / operators
  F3DOBJArchive(F3DOBJArchive const&) = delete;
  void operator=(F3DOBJArchive const&) = delete;

private:
  // Are dataSize and decompressedData valid 
  bool isValid;

  // Archive file name
  std::string archiveName;

  // Buffer information
  std::vector<f3d::scene::bufferTuple> decompressedData;

  // Sub-ranges of decompressed data
  std::vector<f3d::scene::bufferTuple> decompressedModels;
  std::vector<f3d::scene::bufferTuple> decompressedTextures;
  std::vector<f3d::scene::bufferTuple> decompressedMaterials;
  std::vector<f3d::scene::bufferTuple> decompressedOther;

  /**
   * Make sure model files are at the front of the decompressed
   * data vector.
   */
  void SortDecompressedData();

  /**
   * Extract all files from zip archive to memory buffers
   */
  bool DecompressZipToBuffers();

  /**
   * Extract the file at the given index to a memory buffer and return
   */
  static f3d::scene::bufferTuple DecompressIndexToBuffer(const size_t index, zip* za, const zip_stat& st);
};

#endif
