#pragma once

#include <Compression/CompressedReadBufferBase.h>
#include <IO/ReadBufferFromFileBase.h>
#include <IO/ReadSettings.h>
#include <time.h>
#include <memory>


namespace DB
{

class MMappedFileCache;


/// Unlike CompressedReadBuffer, it can do seek.
class CompressedReadBufferFromFile : public CompressedReadBufferBase, public BufferWithOwnMemory<ReadBuffer>
{
private:
      /** At any time, one of two things is true:
      * a) size_compressed = 0
      * b)
      *  - `working_buffer` contains the entire block.
      *  - `file_in` points to the end of this block.
      *  - `size_compressed` contains the compressed size of this block.
      */
    std::unique_ptr<ReadBufferFromFileBase> p_file_in;
    ReadBufferFromFileBase & file_in;
    size_t size_compressed = 0;

    bool nextImpl() override;
    void prefetch() override;

public:
    CompressedReadBufferFromFile(std::unique_ptr<ReadBufferFromFileBase> buf, bool allow_different_codecs_ = false);

    CompressedReadBufferFromFile(
        const std::string & path, const ReadSettings & settings, size_t estimated_size, bool allow_different_codecs_ = false);

    void seek(size_t offset_in_compressed_file, size_t offset_in_decompressed_block);

    size_t readBig(char * to, size_t n) override;

    void setProfileCallback(const ReadBufferFromFileBase::ProfileCallback & profile_callback_, clockid_t clock_type_ = CLOCK_MONOTONIC_COARSE)
    {
        file_in.setProfileCallback(profile_callback_, clock_type_);
    }
};

}
