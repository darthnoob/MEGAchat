#ifndef FILE_LOGGER_INCLUDED
#define FILE_LOGGER_INCLUDED

#include "logger.h"
#include <assert.h>

namespace karere
{
class FileLogger
{
protected:
    FILE* mFile;
    long mRotateSize;
    std::string mFileName;
    volatile unsigned& mFlags;
    long mLogSize;
public:
    void setRotateSize(unsigned rotateSize) { mRotateSize = rotateSize; }

FileLogger(volatile unsigned& flags, const char* logFile, int rotateSize)
 :mFile(NULL), mRotateSize(rotateSize), mFlags(flags), mLogSize(0)
{
    assert(rotateSize > 0);
    if (logFile)
        startLogging(logFile);
}

void startLogging(const char* fileName)
{
	if (mFile)
            fclose(mFile);
	mFileName = fileName;
	openLogFile();
}

void openLogFile()
{
    mFile = fopen(mFileName.c_str(), "ab+");
    if (!mFile)
        throw std::runtime_error("FileLogger: Cannot open file "+mFileName);
    fseek(mFile, 0, SEEK_END);
    mLogSize = ftell(mFile); //in a+ mode the position is at the end of file
}

void logString(const char* buf, size_t len, unsigned flags)
{
//    std::lock_guard<std::mutex> lock(mMutex);
    //do not increment mLogSize until we have actually written the data
    if (mLogSize >= mRotateSize)
        rotateLog();
    mLogSize += len;
    size_t ret = fwrite(buf, 1, len, mFile);
    if (ret != len)
        perror("FileLogger: WARNING: Error writing to log file: ");
    if ((mFlags & krLogNoAutoFlush) == 0)
        fflush(mFile);
}


std::shared_ptr<Logger::LogBuffer> loadLog() //Logger must be locked!!!
{
    std::shared_ptr<Logger::LogBuffer> buf(new Logger::LogBuffer(new char[mLogSize+1], mLogSize+1));
    if (!buf->data)
        throw std::runtime_error("FileLogger::loadLog: Out of memory when allocating buffer");
    fseek(mFile, 0, SEEK_SET);
    long bytesRead = fread(buf->data, 1, mLogSize, mFile);
    if (bytesRead != mLogSize)
    {
        if (ferror(mFile))
            perror("ERROR: FileLogger::loadLog: Error reading log file: ");
        else if (feof(mFile))
            fprintf(stderr, "ERROR: FileLogger::loadLog: EOF while reading log file. Required: %ld, read: %ld", mLogSize, bytesRead);
        else
            fprintf(stderr, "ERROR: FileLogger::loadLog: Unknown error has occurred while reading file. ferror() and feof() were not set");

        return NULL;
    }
    buf->data[mLogSize] = 0; //zero terminate the string in the buffer
    fseek(mFile, 0, SEEK_END);
    return buf;
}

void rotateLog()
{
    auto buf = loadLog();
    long slicePos = mLogSize - (mRotateSize / 2);
    if (slicePos <= 1)
        throw std::runtime_error("FileLogger::rotate: The slice offset is less than 1");
    if (slicePos >= mLogSize - 1)
        throw std::runtime_error("FileLogger::rotate: The slice offset is at the end of the log. Rotate size is too small");

    long i;
    for (i = slicePos; i >= 0; i--)
        if (buf->data[i] == '\n')
            break;
//i shoud point to a \n or be -1
//It is guaranteed that the highest value of i (=half) is
//at least 1 less that the end of the buf (see checks above)
    if (i > 0)
        slicePos = i+1;
//else no new line found in the half->beginning backward scan, so just truncate and dont care about newlines

    fclose(mFile); //writing appends to the end of the file, and now we want to rewrite it
    mFile = NULL;
    FILE* writeFile = fopen(mFileName.c_str(), "wb");
    if (!writeFile)
        throw std::runtime_error("FileLogger::rotate: Cannot open log file for rewriting");
    int writeLen = mLogSize-slicePos;
    int ret = fwrite(buf->data+slicePos, 1, writeLen, writeFile);
    if (ret != writeLen)
    {
        if (ret < 0)
            perror("ERROR: FileLogger::rotate: Error writing file:");
        else
            fprintf(stderr, "ERROR: FileLogger::rotate: Not all data could be written to file: requested %d, written: %d\n",
                writeLen, ret);
    }
    fclose(writeFile);
    openLogFile();
}

~FileLogger()
{
    if (mFile)
        fclose(mFile);
}
};
}
#endif
