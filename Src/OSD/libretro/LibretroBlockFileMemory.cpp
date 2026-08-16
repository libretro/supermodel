#include "LibretroBlockFileMemory.h"

// ==============================
// CBlockFileCounter
// ==============================


CBlockFileCounter::CBlockFileCounter()
    : m_size(0)
    , m_currentBlockDataStart(0)
{
}

size_t CBlockFileCounter::GetSize() const
{
    return m_size;
}

std::vector<LibretroBlockLayout> CBlockFileCounter::GetLayout() const
{
    std::vector<LibretroBlockLayout> layout = m_layout;
    if (!layout.empty())
        layout.back().payloadSize = m_size - m_currentBlockDataStart;
    return layout;
}

void CBlockFileCounter::FinishCurrentBlock()
{
    if (!m_layout.empty())
        m_layout.back().payloadSize = m_size - m_currentBlockDataStart;
}

void CBlockFileCounter::Write(const void* /*data*/, uint32_t numBytes)
{
    m_size += numBytes;
}

void CBlockFileCounter::Write(bool /*value*/)
{
    m_size += 1; // bool stored as 1 byte in BlockFile
}

void CBlockFileCounter::Write(const std::string& str)
{
    // BlockFile writes string INCLUDING null terminator
    m_size += str.size() + 1;
}

// ==============================
// CBlockFileMemory
// ==============================

CBlockFileMemory::CBlockFileMemory(void* data, size_t size)
    : m_ptr(static_cast<uint8_t*>(data))
    , m_offset(0)
    , m_capacity(size)
    , m_currentBlockHeaderPos(0)
    , m_currentBlockEnd(size)
    , m_writable(true)
    , m_error(data == nullptr && size != 0)
{
}

CBlockFileMemory::CBlockFileMemory(const void* data, size_t size)
    : m_ptr(const_cast<uint8_t*>(static_cast<const uint8_t*>(data)))
    , m_offset(0)
    , m_capacity(size)
    , m_currentBlockHeaderPos(0)
    , m_currentBlockEnd(size)
    , m_writable(false)
    , m_error(data == nullptr && size != 0)
{
}

bool CBlockFileMemory::ParseBlock(size_t blockStart,
                                 size_t& blockEnd,
                                 size_t& dataStart,
                                 std::string& blockName) const
{
    if (!m_ptr || blockStart > m_capacity || m_capacity - blockStart < 12)
        return false;

    uint32_t totalBlockLength;
    uint32_t nameLen;
    uint32_t commentLen;
    std::memcpy(&totalBlockLength, m_ptr + blockStart, 4);
    std::memcpy(&nameLen, m_ptr + blockStart + 4, 4);
    std::memcpy(&commentLen, m_ptr + blockStart + 8, 4);

    if (totalBlockLength < 12 || totalBlockLength > m_capacity - blockStart ||
        nameLen == 0 || nameLen > 1025 ||
        commentLen == 0 || commentLen > 1025)
        return false;

    const size_t stringBytes = static_cast<size_t>(nameLen) + commentLen;
    if (stringBytes > totalBlockLength - 12)
        return false;

    const size_t nameStart = blockStart + 12;
    const size_t commentStart = nameStart + nameLen;
    if (m_ptr[nameStart + nameLen - 1] != '\0' ||
        m_ptr[commentStart + commentLen - 1] != '\0')
        return false;

    blockEnd = blockStart + totalBlockLength;
    dataStart = commentStart + commentLen;
    blockName.assign(reinterpret_cast<const char*>(m_ptr + nameStart), nameLen - 1);
    return true;
}

Result CBlockFileMemory::FindBlock(const std::string &name)
{
    if (m_error)
        return Result::FAIL;

    size_t searchOffset = 0; // Local search pointer

    while (searchOffset < m_capacity)
    {
        size_t blockEnd;
        size_t dataStart;
        std::string blockName;
        if (!ParseBlock(searchOffset, blockEnd, dataStart, blockName))
            return Result::FAIL;
        
        // Is this our block?
        if (blockName == name)
        {
            m_offset = dataStart;
            m_currentBlockEnd = blockEnd;
            return Result::OKAY;
        }

        // If not, skip to the next self-describing block.
        searchOffset = blockEnd;
    }
    return Result::FAIL;
}

void CBlockFileMemory::Write(const void* data, uint32_t numBytes)
{
    if (m_error || !m_writable || !data || m_offset > m_capacity ||
        numBytes > m_capacity - m_offset)
    {
        m_error = true;
        return;
    }

    std::memcpy(m_ptr + m_offset, data, numBytes);
    m_offset += numBytes;
}

void CBlockFileMemory::Write(bool value)
{
    uint8_t v = value ? 1 : 0;
    Write(&v, 1);
}

void CBlockFileMemory::Write(const std::string& str)
{
    Write(str.c_str(), static_cast<uint32_t>(str.size() + 1));
}

void CBlockFileCounter::NewBlock(const std::string& name, const std::string& comment) {
    FinishCurrentBlock();
    m_layout.push_back({name, 0});
    m_size += 12 + (name.size() + 1) + (comment.size() + 1);
    m_currentBlockDataStart = m_size;
}

unsigned CBlockFileMemory::Read(void *data, uint32_t numBytes)  {
    if (m_error || !data || m_offset > m_currentBlockEnd ||
        numBytes > m_currentBlockEnd - m_offset)
    {
        m_error = true;
        return 0;
    }
    std::memcpy(data, m_ptr + m_offset, numBytes);
    m_offset += numBytes;
    return numBytes;
}

unsigned CBlockFileMemory::Read(bool *value)  {
    uint8_t v;
    unsigned read = Read(&v, 1);
    if (read == 1) *value = (v != 0);
    return read;
}

void CBlockFileMemory::NewBlock(const std::string& name, const std::string& comment) {
    if (m_error || !m_writable || name.empty() || name.size() > 1024 ||
        comment.size() > 1024)
    {
        m_error = true;
        return;
    }

    // 1. If there was a previous block, update its total length field
    if (m_offset > 0 && m_currentBlockHeaderPos < m_capacity) {
        if (m_offset - m_currentBlockHeaderPos > UINT32_MAX)
        {
            m_error = true;
            return;
        }
        uint32_t totalSize = static_cast<uint32_t>(m_offset - m_currentBlockHeaderPos);
        std::memcpy(m_ptr + m_currentBlockHeaderPos, &totalSize, 4);
    }

    // 2. Record where this new block's length field is located
    m_currentBlockHeaderPos = m_offset;

    // 3. Write the header
    uint32_t dummyTotalLen = 0;
    uint32_t nLen = (uint32_t)name.size() + 1;
    uint32_t cLen = (uint32_t)comment.size() + 1;

    Write(&dummyTotalLen, 4);
    Write(&nLen, 4);
    Write(&cLen, 4);
    Write(name.c_str(), nLen);
    Write(comment.c_str(), cLen);
}

bool CBlockFileMemory::Finish() {
    if (!m_writable)
    {
        m_error = true;
        return false;
    }
    if (!m_error && m_offset > 0) {
        if (m_offset - m_currentBlockHeaderPos > UINT32_MAX)
        {
            m_error = true;
            return false;
        }
        uint32_t totalSize = static_cast<uint32_t>(m_offset - m_currentBlockHeaderPos);
        std::memcpy(m_ptr + m_currentBlockHeaderPos, &totalSize, 4);
    }
    return !m_error;
}

bool CBlockFileMemory::HasError() const
{
    return m_error;
}

size_t CBlockFileMemory::GetOffset() const
{
    return m_offset;
}

bool CBlockFileMemory::ValidateLayout(
    const std::vector<LibretroBlockLayout>& expected) const
{
    size_t offset = 0;

    for (const LibretroBlockLayout& expectedBlock : expected)
    {
        size_t blockEnd;
        size_t dataStart;
        std::string blockName;
        if (!ParseBlock(offset, blockEnd, dataStart, blockName) ||
            blockName != expectedBlock.name ||
            blockEnd - dataStart != expectedBlock.payloadSize)
            return false;
        offset = blockEnd;
    }

    return offset == m_capacity;
}
