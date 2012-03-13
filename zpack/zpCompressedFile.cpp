#include "zpCompressedFile.h"
#include "zpPackage.h"
#include <cassert>
#include "zlib.h"
//#include "PerfUtil.h"

namespace zp
{

////////////////////////////////////////////////////////////////////////////////////////////////////
CompressedFile::CompressedFile(Package* package, u64 offset, u32 compressedSize, u32 originSize, u32 flag)
	: m_package(package)
	, m_offset(offset)
	, m_flag(flag)
	, m_compressedSize(compressedSize)
	, m_originSize(originSize)
	, m_readPos(0)
	, m_chunkPos(NULL)
	, m_fileData(NULL)
	, m_chunkData(NULL)
{
	assert(package != NULL);
	assert(package->m_stream != NULL);

	if (compressedSize <= 0)
	{
		m_originSize = 0;
	}
	m_chunkCount = (m_originSize + m_package->m_header.chunkSize - 1) / m_package->m_header.chunkSize;
	//no chunk size array for files have only 1 chunk
	if (m_chunkCount <= 1)
	{
		return;
	}

	//array of pointer to chunk data buffer
	m_chunkData = new u8*[m_chunkCount];
	memset(m_chunkData, 0, m_chunkCount * sizeof(u8*));
	
	//raw data position of each chunk
	m_chunkPos = new u32[m_chunkCount];
	seekInPackage(0);
	fread((char*)m_chunkPos, m_chunkCount * sizeof(u32), 1, m_package->m_stream);
}

////////////////////////////////////////////////////////////////////////////////////////////////////
CompressedFile::~CompressedFile()
{
	if (m_chunkPos != NULL)
	{
		delete[] m_chunkPos;
		m_chunkPos = NULL;
	}
	if (m_chunkData != NULL)
	{
		for (u32 i = 0; i < m_chunkCount; ++i)
		{
			if (m_chunkData[i] != NULL)
			{
				delete[] m_chunkData[i];
			}
		}
		delete[] m_chunkData;
		m_chunkData = NULL;
	}
	if (m_fileData != NULL)
	{
		delete[] m_fileData;
		m_fileData = NULL;
	}
}

////////////////////////////////////////////////////////////////////////////////////////////////////
u32 CompressedFile::size()
{
	return m_originSize;
}

////////////////////////////////////////////////////////////////////////////////////////////////////
void CompressedFile::seek(u32 pos)
{
	m_readPos = pos;
}

////////////////////////////////////////////////////////////////////////////////////////////////////
u32 CompressedFile::read(u8* buffer, u32 size)
{
	//BEGIN_PERF("read")

	if (m_readPos + size > m_originSize)
	{
		size = m_originSize - m_readPos;
	}
	if (m_originSize == 0)
	{
		return 0;
	}
	if (m_chunkCount == 1)
	{
		size = oneChunkRead(buffer, size);
	}
	else
	{
		//let's do something real!
		u32 startChunk = m_readPos / m_package->m_header.chunkSize;
		u32 endChunk = (m_readPos + size + m_package->m_header.chunkSize - 1) / m_package->m_header.chunkSize;
		u32 dstOffset = 0;
		for (u32 chunkIndex = startChunk; chunkIndex < endChunk; ++chunkIndex)
		{
			u32 readOffset = 0;
			u32 readSize = m_package->m_header.chunkSize;
			if (chunkIndex == startChunk)
			{
				readOffset = m_readPos % m_package->m_header.chunkSize;
			}
			if (chunkIndex == endChunk - 1)
			{
				//last chunk
				readSize = (m_readPos + size) - chunkIndex * m_package->m_header.chunkSize;
			}
			if (!readChunk(chunkIndex, readOffset, readSize, buffer + dstOffset))
			{
				return 0;
			}
			dstOffset += readSize;
		}
	}
	m_readPos += size;

	//END_PERF

	return size;
}

////////////////////////////////////////////////////////////////////////////////////////////////////
u32 CompressedFile::oneChunkRead(u8* buffer, u32 size)
{
	assert(m_readPos + size <= m_originSize);
	assert(m_compressedSize != m_originSize);

	//cached
	if (m_fileData != NULL)
	{
		memcpy(buffer, m_fileData + m_readPos, size);
		return size;
	}

	seekInPackage(0);

	u8* dstBuffer = NULL;
	if (m_readPos == 0 && size == m_originSize)
	{
		//want entire file, no need to cache, just fill user buffer
		dstBuffer = buffer;
	}
	else
	{
		m_fileData = new u8[m_originSize];
		dstBuffer = m_fileData;
	}

	u8* compressed = new u8[m_compressedSize];
	fread((char*)compressed, m_compressedSize, 1, m_package->m_stream);

	u32 dstSize = m_originSize;	//don't want m_originSize to be changed
	if (uncompress(dstBuffer, &dstSize, compressed, m_compressedSize) != Z_OK)
	{
		size = 0;
	}
	if (m_fileData != NULL && size > 0)
	{
		memcpy(buffer, m_fileData + m_readPos, size);
	}
	delete[] compressed;
	return size;
}

////////////////////////////////////////////////////////////////////////////////////////////////////
bool CompressedFile::readChunk(u32 chunkIndex, u32 offset, u32 readSize, u8* buffer)
{
	assert(m_chunkData != NULL);

	if (m_chunkData[chunkIndex] != NULL)
	{
		//cached
		memcpy(buffer, m_chunkData[chunkIndex] + offset, readSize);
		return true;
	}

	assert(m_chunkPos != NULL);
	seekInPackage(m_chunkPos[chunkIndex]);

	u32 compressedChunkSize = 0;
	u32 originChunkSize = 0;
	if (chunkIndex + 1 < m_chunkCount)
	{
		compressedChunkSize = m_chunkPos[chunkIndex + 1] - m_chunkPos[chunkIndex];
		originChunkSize = m_package->m_header.chunkSize;
	}
	else
	{
		//last chunk
		compressedChunkSize = m_compressedSize - m_chunkPos[m_chunkCount - 1];
		originChunkSize = m_originSize % m_package->m_header.chunkSize;
	}

	u8* dstBuffer = NULL;
	if (offset == 0 && readSize == originChunkSize)
	{
		//want entire chunk, no need to cache, just fill user buffer
		dstBuffer = buffer;
	}
	else
	{
		m_chunkData[chunkIndex] = new u8[originChunkSize];
		dstBuffer = m_chunkData[chunkIndex];
	}

	if (compressedChunkSize == originChunkSize)
	{
		//this chunk was not compressed at all, read directly to the dstBuffer
		fread((char*)dstBuffer, originChunkSize, 1, m_package->m_stream);
	}
	else
	{
		u8* compressed = new u8[compressedChunkSize];
		fread((char*)compressed, compressedChunkSize, 1, m_package->m_stream);

		int ret = uncompress(dstBuffer, &originChunkSize, compressed, compressedChunkSize);
		delete[] compressed;
		if (ret != Z_OK)
		{
			return false;
		}
	}
	if (m_chunkData[chunkIndex] != NULL)
	{
		memcpy(buffer, m_chunkData[chunkIndex] + offset, readSize);
	}
	return true;
}

////////////////////////////////////////////////////////////////////////////////////////////////////
void CompressedFile::seekInPackage(u32 offset)
{
	_fseeki64(m_package->m_stream, m_offset + offset, SEEK_SET);
	m_package->m_lastSeekFile = this;
}

}