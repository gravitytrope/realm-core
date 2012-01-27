#include "Array.h"
#include <cassert>
#include "Column.h"
#include "utilities.h"
#ifdef _MSC_VER
	#include "win32\types.h"
#endif

// Pre-declare local functions
size_t CalcByteLen(size_t count, size_t width);

Array::Array(size_t ref, Array* parent, size_t pndx, Allocator& alloc)
: m_data(NULL), m_len(0), m_capacity(0), m_width(0), m_isNode(false), m_hasRefs(false), m_parent(parent), m_parentNdx(pndx), m_alloc(alloc), m_lbound(0), m_ubound(0) {
	Create(ref);
}

Array::Array(size_t ref, const Array* parent, size_t pndx, Allocator& alloc)
: m_data(NULL), m_len(0), m_capacity(0), m_width(0), m_isNode(false), m_hasRefs(false), m_parent(const_cast<Array*>(parent)), m_parentNdx(pndx), m_alloc(alloc), m_lbound(0), m_ubound(0) {
	Create(ref);
}

Array::Array(ColumnDef type, Array* parent, size_t pndx, Allocator& alloc)
: m_data(NULL), m_len(0), m_capacity(0), m_width(-1), m_isNode(false), m_hasRefs(false), m_parent(parent), m_parentNdx(pndx), m_alloc(alloc), m_lbound(0), m_ubound(0) {
	if (type == COLUMN_NODE) m_isNode = m_hasRefs = true;
	else if (type == COLUMN_HASREFS)    m_hasRefs = true;

	Alloc(0, 0);
	SetWidth(0);
}

// Creates new array (but invalid, call UpdateRef or SetType to init)
Array::Array(Allocator& alloc)
: m_ref(0), m_data(NULL), m_len(0), m_capacity(0), m_width(-1), m_parent(NULL), m_parentNdx(0), m_alloc(alloc) {
}

// Copy-constructor
// Note that this array now own the ref. Should only be used when
// the source array goes away right after (like return values from functions)
Array::Array(const Array& src) : m_parent(src.m_parent), m_parentNdx(src.m_parentNdx), m_alloc(src.m_alloc) {
	const size_t ref = src.GetRef();
	Create(ref);
	src.Invalidate();
}

Array::~Array() {}

// Header format (8 bytes):
// |--------|--------|--------|--------|--------|--------|--------|--------|
// |12-33444|          length          |         capacity         |reserved|
//
//  1: isNode  2: hasRefs  3: multiplier  4: width (packed in 3 bits)

void Array::set_header_isnode(bool value, void* header) {
	uint8_t* const header2 = header ? (uint8_t*)header : (m_data - 8);
	header2[0] = (header2[0] & (~0x80)) | ((uint8_t)value << 7);
}
void Array::set_header_hasrefs(bool value, void* header) {
	uint8_t* const header2 = header ? (uint8_t*)header : (m_data - 8);
	header2[0] = (header2[0] & (~0x40)) | ((uint8_t)value << 6);
}

void Array::set_header_wtype(WidthType value, void* header) {
	// Indicates how to calculate size in bytes based on width
	// 0: bits      (width/8) * length
	// 1: multiply  width * length
	// 2: ignore    1 * length
	uint8_t* const header2 = header ? (uint8_t*)header : (m_data - 8);
	header2[0] = (header2[0] & (~0x18)) | ((uint8_t)value << 3);
}

void Array::set_header_width(size_t value, void* header) {
	// Pack width in 3 bits (log2)
	unsigned int w = 0;
	unsigned int b = (unsigned int)value;
	while (b) {++w; b >>= 1;}
	assert(w < 8);

	uint8_t* const header2 = header ? (uint8_t*)header : (m_data - 8);
	header2[0] = (header2[0] & (~0x7)) | (uint8_t)w;
}
void Array::set_header_len(size_t value, void* header) {
	assert(value <= 0xFFFFFF);
	uint8_t* const header2 = header ? (uint8_t*)header : (m_data - 8);
	header2[1] = ((value >> 16) & 0x000000FF);
	header2[2] = (value >> 8) & 0x000000FF;
	header2[3] = value & 0x000000FF;
}
void Array::set_header_capacity(size_t value, void* header) {
	assert(value <= 0xFFFFFF);
	uint8_t* const header2 = header ? (uint8_t*)header : (m_data - 8);
	header2[4] = (value >> 16) & 0x000000FF;
	header2[5] = (value >> 8) & 0x000000FF;
	header2[6] = value & 0x000000FF;
}

bool Array::get_header_isnode(const void* header) const {
	const uint8_t* const header2 = header ? (const uint8_t*)header : (m_data - 8);
	return (header2[0] & 0x80) != 0;
}
bool Array::get_header_hasrefs(const void* header) const {
	const uint8_t* const header2 = header ? (const uint8_t*)header : (m_data - 8);
	return (header2[0] & 0x40) != 0;
}
Array::WidthType Array::get_header_wtype(const void* header) const {
	const uint8_t* const header2 = header ? (const uint8_t*)header : (m_data - 8);
	return (WidthType)((header2[0] & 0x18) >> 3);
}
size_t Array::get_header_width(const void* header) const {
	const uint8_t* const header2 = header ? (const uint8_t*)header : (m_data - 8);
	return (1 << (header2[0] & 0x07)) >> 1;
}
size_t Array::get_header_len(const void* header) const {
	const uint8_t* const header2 = header ? (const uint8_t*)header : (m_data - 8);
	return (header2[1] << 16) + (header2[2] << 8) + header2[3];
}
size_t Array::get_header_capacity(const void* header) const {
	const uint8_t* const header2 = header ? (const uint8_t*)header : (m_data - 8);
	return (header2[4] << 16) + (header2[5] << 8) + header2[6];
}

void Array::Create(size_t ref) {
	assert(ref);
	uint8_t* const header = (uint8_t*)m_alloc.Translate(ref);

	// Parse header
	m_isNode   = get_header_isnode(header);
	m_hasRefs  = get_header_hasrefs(header);
	m_width    = get_header_width(header);
	m_len      = get_header_len(header);
	const size_t byte_capacity = get_header_capacity(header);

	// Capacity is how many items there are room for
	m_capacity = CalcItemCount(byte_capacity, m_width);

	m_ref = ref;
	m_data = header + 8;

	SetWidth(m_width);
}

void Array::SetType(ColumnDef type) {
	if (m_ref) CopyOnWrite();

	if (type == COLUMN_NODE) m_isNode = m_hasRefs = true;
	else if (type == COLUMN_HASREFS)    m_hasRefs = true;
	else m_isNode = m_hasRefs = false;

	if (!m_data) {
		// Create array
		Alloc(0, 0);
		SetWidth(0);
	}
	else {
		// Update Header
		set_header_isnode(m_isNode);
		set_header_hasrefs(m_hasRefs);
	}
}

bool Array::operator==(const Array& a) const {
	return m_data == a.m_data;
}

void Array::UpdateRef(size_t ref) {
	Create(ref);

	// Update ref in parent
	if (m_parent) m_parent->Set(m_parentNdx, ref);
}

/**
 * Takes a 64bit value and return the minimum number of bits needed to fit the
 * value.
 * For alignment this is rounded up to nearest log2.
 * Posssible results {0, 1, 2, 4, 8, 16, 32, 64}
 */
static unsigned int BitWidth(int64_t v) {
	if ((v >> 4) == 0) {
		static const int8_t bits[] = {0, 1, 2, 2, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4};
		return bits[(int8_t)v];
	}

	// First flip all bits if bit 63 is set (will now always be zero)
	if (v < 0) v = ~v;

	// Then check if bits 15-31 used (32b), 7-31 used (16b), else (8b)
	return v >> 31 ? 64 : v >> 15 ? 32 : v >> 7 ? 16 : 8;
}

void Array::SetParent(Array* parent, size_t pndx) {
	m_parent = parent;
	m_parentNdx = pndx;
}

Array Array::GetSubArray(size_t ndx) {
	assert(ndx < m_len);
	assert(m_hasRefs);

	const size_t ref = (size_t)Get(ndx);
	assert(ref);

	return Array(ref, this, ndx, m_alloc);
}

const Array Array::GetSubArray(size_t ndx) const {
	assert(ndx < m_len);
	assert(m_hasRefs);

	return Array((size_t)Get(ndx), this, ndx, m_alloc);
}

void Array::Destroy() {
	if (!m_data) return;

	if (m_hasRefs) {
		for (size_t i = 0; i < m_len; ++i) {
			const size_t ref = (size_t)Get(i);

			// null-refs signify empty sub-trees
			if (ref == 0) continue;

			// all refs are 64bit aligned, so the lowest bits
			// cannot be set. If they are it means that it should
			// not be interpreted as a ref
			if (ref & 0x1) continue;

			Array sub(ref, this, i, m_alloc);
			sub.Destroy();
		}
	}

	void* ref = m_data-8;
	m_alloc.Free(m_ref, ref);
	m_data = NULL;
}

void Array::Clear() {
	CopyOnWrite();

	// Make sure we don't have any dangling references
	if (m_hasRefs) {
		for (size_t i = 0; i < Size(); ++i) {
			Array sub((size_t)Get(i), this, i);
			sub.Destroy();
		}
	}

	// Truncate size to zero (but keep capacity)
	m_len      = 0;
	m_capacity = CalcItemCount(get_header_capacity(), 0);
	SetWidth(0);

	// Update header
	set_header_len(0);
	set_header_width(0);
}

void Array::Delete(size_t ndx) {
	assert(ndx < m_len);

	// Check if we need to copy before modifying
	CopyOnWrite();

	// Move values below deletion up
	if (m_width < 8) {
		for (size_t i = ndx+1; i < m_len; ++i) {
			const int64_t v = (this->*m_getter)(i);
			(this->*m_setter)(i-1, v);
		}
	}
	else if (ndx < m_len-1) {
		// when byte sized, use memmove
		const size_t w = (m_width == 64) ? 8 : (m_width == 32) ? 4 : (m_width == 16) ? 2 : 1;
		unsigned char* dst = m_data + (ndx * w);
		unsigned char* src = dst + w;
		const size_t count = (m_len - ndx - 1) * w;
		memmove(dst, src, count);
	}

	// Update length (also in header)
	--m_len;
	set_header_len(m_len);
}

int64_t Array::Get(size_t ndx) const {
	assert(ndx < m_len);
	return (this->*m_getter)(ndx);
}

int64_t Array::Back() const {
	assert(m_len);
	return (this->*m_getter)(m_len-1);
}



void Array::SetBounds(size_t width) {
	if(width == 0) {
		m_lbound = 0;
		m_ubound = 0;
	}
	else if(width == 1) {
		m_lbound = 0;
		m_ubound = 1;
	}
	else if(width == 2) {
		m_lbound = 0;
		m_ubound = 3;
	}
	else if(width == 4) {
		m_lbound = 0;
		m_ubound = 15;
	}
	else if(width == 8) {
		m_lbound = -0x80LL;
		m_ubound =  0x7FLL;
	}
	else if(width == 16) {
		m_lbound = -0x8000LL;
		m_ubound =  0x7FFFLL;
	}
	else if(width == 32) {
		m_lbound = -0x80000000LL;
		m_ubound =  0x7FFFFFFFLL;
	}
	else if(width == 64) {
		m_lbound = -0x8000000000000000LL;
		m_ubound =  0x7FFFFFFFFFFFFFFFLL;
	}
}



bool Array::Set(size_t ndx, int64_t value) {
	assert(ndx < m_len);

	// Check if we need to copy before modifying
	if (!CopyOnWrite()) return false;

	// Make room for the new value
	size_t width = m_width;

	if(value < m_lbound || value > m_ubound)
		width = BitWidth(value);

	const bool doExpand = (width > m_width);
	if (doExpand) {

		Getter oldGetter = m_getter;
		if (!Alloc(m_len, width)) return false;
		SetWidth(width);

		// Expand the old values
		int k = (int)m_len;
		while (--k >= 0) {
			const int64_t v = (this->*oldGetter)(k);
			(this->*m_setter)(k, v);
		}
	}

	// Set the value
	(this->*m_setter)(ndx, value);

	return true;
}

// Optimization for the common case of adding
// positive values to a local array (happens a
// lot when returning results to TableViews)
bool Array::AddPositiveLocal(int64_t value) {
	assert(value >= 0);
	assert(&m_alloc == &GetDefaultAllocator());

	if (value <= m_ubound) {
		if (m_len < m_capacity) {
			(this->*m_setter)(m_len, value);
			++m_len;
			set_header_len(m_len);
			return true;
		}
	}

	return Insert(m_len, value);
}

bool Array::Insert(size_t ndx, int64_t value) {
	assert(ndx <= m_len);

	// Check if we need to copy before modifying
	if (!CopyOnWrite()) return false;

	Getter getter = m_getter;

	// Make room for the new value
	size_t width = m_width; 

	if(value < m_lbound || value > m_ubound)
		width = BitWidth(value);
	
	const bool doExpand = (width > m_width);
	if (doExpand) {
		if (!Alloc(m_len+1, width)) return false;
		SetWidth(width);
	}
	else {
		if (!Alloc(m_len+1, m_width)) return false;
	}

	// Move values below insertion (may expand)
	if (doExpand || m_width < 8) {
		int k = (int)m_len;
		while (--k >= (int)ndx) {
			const int64_t v = (this->*getter)(k);
			(this->*m_setter)(k+1, v);
		}
	}
	else if (ndx != m_len) {
		// when byte sized and no expansion, use memmove
		const size_t w = (m_width == 64) ? 8 : (m_width == 32) ? 4 : (m_width == 16) ? 2 : 1;
		unsigned char* src = m_data + (ndx * w);
		unsigned char* dst = src + w;
		const size_t count = (m_len - ndx) * w;
		memmove(dst, src, count);
	}

	// Insert the new value
	(this->*m_setter)(ndx, value);

	// Expand values above insertion
	if (doExpand) {
		int k = (int)ndx;
		while (--k >= 0) {
			const int64_t v = (this->*getter)(k);
			(this->*m_setter)(k, v);
		}
	}

	// Update length
	// (no need to do it in header as it has been done by Alloc)
	++m_len;

	return true;
}


bool Array::Add(int64_t value) {
	return Insert(m_len, value);
}

void Array::Resize(size_t count) {
	assert(count <= m_len);

	CopyOnWrite();

	// Update length (also in header)
	m_len = count;
	set_header_len(m_len);
}

bool Array::Increment(int64_t value, size_t start, size_t end) {
	if (end == (size_t)-1) end = m_len;
	assert(start < m_len);
	assert(end >= start && end <= m_len);

	// Increment range
	for (size_t i = start; i < end; ++i) {
		Set(i, Get(i) + value);
	}
	return true;
}

bool Array::IncrementIf(int64_t limit, int64_t value) {
	// Update (incr or decrement) values bigger or equal to the limit
	for (size_t i = 0; i < m_len; ++i) {
		const int64_t v = Get(i);
		if (v >= limit) Set(i, v + value);
	}
	return true;
}

void Array::Adjust(size_t start, int64_t diff) {
	assert(start <= m_len);

	for (size_t i = start; i < m_len; ++i) {
		const int64_t v = Get(i);
		Set(i, v + diff);
	}
}

size_t Array::FindPos(int64_t target) const {
	int low = -1;
	int high = (int)m_len;

	// Binary search based on:
	// http://www.tbray.org/ongoing/When/200x/2003/03/22/Binary
	// Finds position of largest value SMALLER than the target (for lookups in
	// nodes)
	while (high - low > 1) {
		const size_t probe = ((unsigned int)low + (unsigned int)high) >> 1;
		const int64_t v = (this->*m_getter)(probe);

		if (v > target) high = (int)probe;
		else            low = (int)probe;
	}
	if (high == (int)m_len) return (size_t)-1;
	else return high;
}

size_t Array::FindPos2(int64_t target) const {
	int low = -1;
	int high = (int)m_len;

	// Binary search based on:
	// http://www.tbray.org/ongoing/When/200x/2003/03/22/Binary
	// Finds position of closest value BIGGER OR EQUAL to the target (for
	// lookups in indexes)
	while (high - low > 1) {
		const size_t probe = ((unsigned int)low + (unsigned int)high) >> 1;
		const int64_t v = (this->*m_getter)(probe);

		if (v < target) low = (int)probe;
		else            high = (int)probe;
	}
	if (high == (int)m_len) return (size_t)-1;
	else return high;
}

size_t Array::Find(int64_t value, size_t start, size_t end) const {
#ifdef USE_SSE
	if(end == -1)
		end = m_len;

	if(end - start < sizeof(__m128i) || m_width < 8 || m_width == 64) 
		return FindNaive(value, start, end);

	// FindSSE() must start at 16-byte boundary, so search area before that using FindNaive()
	__m128i *a = (__m128i *)round_up(m_data + start * m_width / 8, sizeof(__m128i));
	__m128i *b = (__m128i *)round_down(m_data + end * m_width / 8, sizeof(__m128i));
	size_t t = 0;

	t = FindNaive(value, start, ((unsigned char *)a - m_data) * 8 / m_width);
	if(t != -1)
		return t;

	// Search aligned area with SSE
	if(b > a) {
		t = FindSSE(value, a, m_width / 8, b - a);
		if(t != -1) {
			// FindSSE returns SSE chunk number, so we use FindNative() to find packed position
			t = FindNaive(value, t * sizeof(__m128i) * 8 / m_width  +  (((unsigned char *)a - m_data) / m_width), end);
			return t;
		}
	}

	// Search remainder with FindNaive()
	t = FindNaive(value, ((unsigned char *)b - m_data) * 8 / m_width, end);
	return t;
#else
	return FindNaive(value, start, end); // enable legacy find
#endif
}

#ifdef USE_SSE
// 'items' is the number of 16-byte SSE chunks. 'bytewidth' is the size of a packed data element.
// Return value is SSE chunk number where the element is guaranteed to exist (use FindNative() to
// find packed position)
size_t Array::FindSSE(int64_t value, __m128i *data, size_t bytewidth, size_t items) const{
	__m128i search, next, compare = {1};
	size_t i;

	for(int j = 0; j < sizeof(__m128i) / bytewidth; j++)
		memcpy((char *)&search + j * bytewidth, &value, bytewidth);

	if(bytewidth == 1) {
		for(i = 0; i < items && _mm_movemask_epi8(compare) == 0; i++) {
			next = _mm_load_si128(&data[i]);
			compare = _mm_cmpeq_epi8(search, next);
		}
	}
	else if(bytewidth == 2) {
		for(i = 0; i < items && _mm_movemask_epi8(compare) == 0; i++) {
			next = _mm_load_si128(&data[i]);
			compare = _mm_cmpeq_epi16(search, next);
		}
	}
	else if(bytewidth == 4) {
		for(i = 0; i < items && _mm_movemask_epi8(compare) == 0; i++) {
			next = _mm_load_si128(&data[i]);
			compare = _mm_cmpeq_epi32(search, next);
		}
	}

	// Only supported by SSE 4.1. We use SSE 2 instead which is default in gcc (no -sse41 flag needed) and VC
/*	else if(bytewidth == 8) {
		for(i = 0; i < items && _mm_movemask_epi8(compare) == 0; i++) {
			next = _mm_load_si128(&data[i]);
			compare = _mm_cmpeq_epi64(search, next);
		}
	}*/
	return _mm_movemask_epi8(compare) == 0 ? -1 : i - 1;
}
#endif //USE_SSE

size_t Array::FindNaive(int64_t value, size_t start, size_t end) const {
	if (IsEmpty()) return (size_t)-1;
	if (end == (size_t)-1) end = m_len;
	if (start == end) return (size_t)-1;
		
	assert(start < m_len && end <= m_len && start < end);

	// If the value is wider than the column
	// then we know it can't be there
	const size_t width = BitWidth(value);
	if (width > m_width) return (size_t)-1;

	// Do optimized search based on column width
	if (m_width == 0) {
		return start; // value can only be zero
	}
	else if (m_width == 2) {
		// Create a pattern to match 64bits at a time
		const int64_t v = ~0ULL/0x3 * value;

		const int64_t* p = (const int64_t*)(m_data + start * m_width / 8);
		const int64_t* const e = (const int64_t*)(m_data + end * m_width / 8);

		// Check 64bits at a time for match
		while (p < e) {
			const uint64_t v2 = *p ^ v; // zero matching bit segments
			const uint64_t hasZeroByte = (v2 - 0x5555555555555555UL) & ~v2
											 & 0xAAAAAAAAAAAAAAAAUL;
			if (hasZeroByte) break;
			++p;
		}

		// Position of last chunk (may be partial)
		size_t i = (p - (const int64_t*)m_data) * 32;
		if(i < start) i = start;

		// Manually check the rest
		while (i < end) {
			if (Get(i) == value) return i;
			++i;
		}
	}
	else if (m_width == 4) {
		// Create a pattern to match 64bits at a time
		const int64_t v = ~0ULL/0xF * value;

		const int64_t* p = (const int64_t*)(m_data + start * m_width / 8);
		const int64_t* const e = (const int64_t*)(m_data + end * m_width / 8);

		// Check 64bits at a time for match
		while (p < e) {
			const uint64_t v2 = *p ^ v; // zero matching bit segments
			const uint64_t hasZeroByte = (v2 - 0x1111111111111111UL) & ~v2 
											 & 0x8888888888888888UL;
			if (hasZeroByte) break;
			++p;
		}

		// Position of last chunk (may be partial)
		size_t i = (p - (const int64_t*)m_data) * 16;
		if(i < start) i = start;

		// Manually check the rest
		while (i < end) {
			if (Get(i) == value) return i;
			++i;
		}
	}
  else if (m_width == 8) {
		// TODO: Handle partial searches

		// Create a pattern to match 64bits at a time
		const int64_t v = ~0ULL/0xFF * value;

		const int64_t* p = (const int64_t*)(m_data + start * m_width / 8);
		const int64_t* const e = (const int64_t*)(m_data + end * m_width / 8);

		// Check 64bits at a time for match
		while (p < e) {
			const uint64_t v2 = *p ^ v; // zero matching bit segments
			const uint64_t hasZeroByte = (v2 - 0x0101010101010101ULL) & ~v2
											 & 0x8080808080808080ULL;
			if (hasZeroByte) break;
			++p;
		}

		// Position of last chunk (may be partial)
		size_t i = (p - (const int64_t*)m_data) * 8;
		if(i < start) i = start;

		// Manually check the rest
		while (i < end) {
			if (value == Get(i)) return i;
			++i;
		}
	}
	else if (m_width == 16) {
		// Create a pattern to match 64bits at a time
		const int64_t v = ~0ULL/0xFFFF * value;

		const int64_t* p = (const int64_t*)(m_data + start * m_width / 8);
		const int64_t* const e = (const int64_t*)(m_data + end * m_width / 8);

		// Check 64bits at a time for match
		while (p < e) {
			const uint64_t v2 = *p ^ v; // zero matching bit segments
			const uint64_t hasZeroByte = (v2 - 0x0001000100010001UL) & ~v2
											 & 0x8000800080008000UL;
			if (hasZeroByte) break;
			++p;
		}
		
		// Position of last chunk (may be partial)
		size_t i = (p - (const int64_t*)m_data) * 4;
		if(i < start) i = start;

		// Manually check the rest
		while (i < end) {
			if (value == Get(i)) return i;
			++i;
		}
	}
	else if (m_width == 32) {
		// Create a pattern to match 64bits at a time
		const int64_t v = ~0ULL/0xFFFFFFFF * value;

		const int64_t* p = (const int64_t*)(m_data + start * m_width / 8);
		const int64_t* const e = (const int64_t*)(m_data + end * m_width / 8);

		// Check 64bits at a time for match
		while (p < e) {
			const uint64_t v2 = *p ^ v; // zero matching bit segments
			const uint64_t hasZeroByte = (v2 - 0x0000000100000001UL) & ~v2
											 & 0x8000800080000000UL;
			if (hasZeroByte) break;
			++p;
		}
		
		// Position of last chunk (may be partial)
		size_t i = (p - (const int64_t*)m_data) * 2;
		if(i < start) i = start;

		// Manually check the rest
		while (i < end) {
			if (value == Get(i)) return i;
			++i;
		}
	}
	else if (m_width == 64) {
		const int64_t v = (int64_t)value;
		const int64_t* p = (const int64_t*)(m_data + start * m_width / 8);
		const int64_t* const e = (const int64_t*)(m_data + end * m_width / 8);
		while (p < e) {
			if (*p == v) return p - (const int64_t*)m_data;
			++p;
		}
	}
	else {
		// Naive search
		for (size_t i = start; i < end; ++i) {
			const int64_t v = Get(i);
			if (v == value) return i;
		}
	}

	return (size_t)-1; // not found
}

void Array::FindAll(Array& result, int64_t value, size_t colOffset,
					size_t start, size_t end) const {
	if (IsEmpty()) return;
	if (end == (size_t)-1) end = m_len;
	if (start == end) return;

	assert(start < m_len && end <= m_len && start < end);

	// If the value is wider than the column
	// then we know it can't be there
	const size_t width = BitWidth(value);
	if (width > m_width) return;

	// Do optimized search based on column width
	if (m_width == 0) {
		for(size_t i = start; i < end; i++){
			result.AddPositiveLocal(i + colOffset); // All values can only be zero.
		}
	}
	else if (m_width == 2) {
		// Create a pattern to match 64bits at a time
		const int64_t v = ~0ULL/0x3 * value;

		const int64_t* p = (const int64_t*)m_data + start;
		const size_t end64 = m_len / 32;
		const int64_t* const e = (const int64_t*)m_data + end64;

		// Check 64bits at a time for match
		while (p < e) {
			const uint64_t v2 = *p ^ v; // zero matching bit segments
			const uint64_t hasZeroByte = (v2 - 0x5555555555555555UL) & ~v2 
										 & 0xAAAAAAAAAAAAAAAAUL;
			if (hasZeroByte){
				// Element number at start of block
				size_t i = (p - (const int64_t*)m_data) * 32;
				const size_t j = i + 32; // Last element of block

				// check block
				while (i < j) {
					const size_t offset = i >> 2;
					const int64_t v = (m_data[offset] >> ((i & 3) << 1)) & 0x03;
					if (v == value) result.AddPositiveLocal(i + colOffset);
					++i;
				}
			}
			++p;
		}

		// Position of last chunk (may be partial)
		size_t i = (p - (const int64_t*)m_data) * 32;

		// Manually check the rest
		while (i < end) {
			if (Get(i) == value) result.AddPositiveLocal(i + colOffset);
			++i;
		}
	}
	else if (m_width == 4) {
		// Create a pattern to match 64bits at a time
		const int64_t v = ~0ULL/0xF * value;

		const int64_t* p = (const int64_t*)m_data + start;
		const size_t end64 = m_len / 16;
		const int64_t* const e = (const int64_t*)m_data + end64;

		// Check 64bits at a time for match
		while (p < e) {
			const uint64_t v2 = *p ^ v; // zero matching bit segments
			const uint64_t hasZeroByte = (v2 - 0x1111111111111111UL) & ~v2 
										 & 0x8888888888888888UL;
			if (hasZeroByte){
				// Element number at start of block
				size_t i = (p - (const int64_t*)m_data) * 16;
				const size_t j = i + 16; // Last element of block

				// check block
				while (i < j) {
					const size_t offset = i >> 1;
					const int64_t v = (m_data[offset] >> ((i & 1) << 2)) & 0xF;
					if (v == value) result.AddPositiveLocal(i + colOffset);
					++i;
				}
			}
			++p;
		}

		// Position of last chunk (may be partial)
		size_t i = (p - (const int64_t*)m_data) * 16;

		// Manually check the rest
		while (i < end) {
			if (Get(i) == value) result.AddPositiveLocal(i + colOffset);
			++i;
		}
	}
	else if (m_width == 8) {
		// TODO: Handle partial searches

		// Create a pattern to match 64bits at a time
		const int64_t v = ~0ULL/0xFF * value;

		const int64_t* p = (const int64_t*)m_data + start;
		const size_t end64 = m_len / 8;
		const int64_t* const e = (const int64_t*)m_data + end64;

		// Check 64bits at a time for match
		while (p < e) {
			const uint64_t v2 = *p ^ v; // zero matching bit segments
			const uint64_t hasZeroByte = (v2 - 0x0101010101010101ULL) & ~v2
											 & 0x8080808080808080ULL;
			if (hasZeroByte){
				// Element number at start of block
				size_t i = (p - (const int64_t*)m_data) * 8;
				const size_t j = i + 8; // Last element of block
				const int8_t* const d = (const int8_t*)m_data; // Data pointer

				// check block
				while (i < j) {
					if (value == d[i]) result.AddPositiveLocal(i + colOffset);
					++i;
				}
			}
			++p;
		}

		// Position of last chunk (may be partial)
		size_t i = (p - (const int64_t*)m_data) * 8;
		// Manually check the rest
		while (i < end) {
			if (value == Get(i)) result.AddPositiveLocal(i + colOffset);
			++i;
		}
	}
	else if (m_width == 16) {
		// Create a pattern to match 64bits at a time
		const int64_t v = ~0ULL/0xFFFF * value;

		const int64_t* p = (const int64_t*)m_data + start;
		const size_t end64 = m_len / 4;
		const int64_t* const e = (const int64_t*)m_data + end64;

		// Check 64bits at a time for match
		while (p < e) {
			const uint64_t v2 = *p ^ v; // zero matching bit segments
			const uint64_t hasZeroByte = (v2 - 0x0001000100010001UL) & ~v2
											 & 0x8000800080008000UL;
			if (hasZeroByte){
				// Element number at start of block
				size_t i = (p - (const int64_t*)m_data) * 4;
				const size_t j = i + 4; // Last element of block
				const int16_t* const d = (const int16_t*)m_data; // Data pointer

				// check block
				while (i < j) {
					if (value == d[i]) result.AddPositiveLocal(i + colOffset);
					++i;
				}
			}
			++p;
		}

		// Position of last chunk (may be partial)
		size_t i = (p - (const int64_t*)m_data) * 4;

		// Manually check the rest
		while (i < end) {
			if (value == Get(i))
				result.AddPositiveLocal(i + colOffset);
			++i;
		}
	}
	else if (m_width == 32) {
		// Create a pattern to match 64bits at a time
		const int64_t v = ~0ULL/0xFFFFFFFF * value;

		const int64_t* p = (const int64_t*)m_data + start;
		const size_t end64 = m_len / 2;
		const int64_t* const e = (const int64_t*)m_data + end64;

		// Check 64bits at a time for match
		while (p < e) {
			const uint64_t v2 = *p ^ v; // zero matching bit segments
			const uint64_t hasZeroByte = (v2 - 0x0000000100000001UL) & ~v2
											 & 0x8000800080000000UL;
			if (hasZeroByte){
				size_t i = (p - (const int64_t*)m_data) * 2;     // Element number at start of block
				const size_t j = i + 2;                          // Last element of block
				const int32_t* const d = (const int32_t*)m_data; // Data pointer

				// check block
				while (i < j) {
					if (value == d[i]) result.AddPositiveLocal(i + colOffset);
					++i;
				}
			}
			++p;
		}

		// Position of last chunk (may be partial)
		size_t i = (p - (const int64_t*)m_data) * 2;

		// Manually check the rest
		while (i < end) {
			if (value == Get(i)) result.AddPositiveLocal(i + colOffset);
			++i;
		}
	}
	else if (m_width == 64) {
		const int64_t v = (int64_t)value;
		const int64_t* p = (const int64_t*)m_data + start;
		const int64_t* const e = (const int64_t*)m_data + end;
		while (p < e) {
			if (*p == v) result.AddPositiveLocal((p - (const int64_t*)m_data) + colOffset);
			++p;
		}
	}
	else {
		// Naive search
		for (size_t i = start; i < end; ++i) {
			const int64_t v = (this->*m_getter)(i);
			if (v == value) result.AddPositiveLocal(i + colOffset);
		}
	}
}


bool Array::Max(int64_t& result, size_t start, size_t end) const {
	if (end == (size_t)-1) end = m_len;
	if (start == end) return false;
	assert(start < m_len && end <= m_len && start < end);
	if (m_width == 0) {result = 0; return true;} // max value is zero

	result = Get(start);

	for (size_t i = start+1; i < end; ++i) {
		const int64_t v = Get(i);
		if (v > result) {
			result = v;
		}
	}

	return true;
}


bool Array::Min(int64_t& result, size_t start, size_t end) const {
	if (end == (size_t)-1) end = m_len;
	if (start == end) return false;
	assert(start < m_len && end <= m_len && start < end);
	if (m_width == 0) {result = 0; return true;} // min value is zero

	result = Get(start);

	for (size_t i = start+1; i < end; ++i) {
		const int64_t v = Get(i);
		if (v < result) {
			result = v;
		}
	}

	return true;
}


int64_t Array::Sum(size_t start, size_t end) const {
	if (IsEmpty()) return 0;
	if (end == (size_t)-1) end = m_len;
	if (start == end) return 0;
	assert(start < m_len && end <= m_len && start < end);

	uint64_t sum = 0;
	
	if (m_width == 0)
		return 0;
	else if( m_width == 8) {
		for (size_t i = start; i < end; ++i)
			sum += Get_8b(i);
	}
	else if (m_width == 16) {
		for (size_t i = start; i < end; ++i)
			sum += Get_16b(i);
	}
	else if(m_width == 32) {
		for (size_t i = start; i < end; ++i)
			sum += Get_32b(i);
	}
	else if(m_width == 64) {
		for (size_t i = start; i < end; ++i)
			sum += Get_64b(i);
	}
	else {
		// Sum of bitwidths less than a byte (which are always positive)
		// uses a divide and conquer algorithm that is a variation of popolation count:
		// http://graphics.stanford.edu/~seander/bithacks.html#CountBitsSetParallel

		// staiic values needed for fast sums
		const uint64_t m1  = 0x5555555555555555;
		const uint64_t m2  = 0x3333333333333333;
		const uint64_t m4  = 0x0f0f0f0f0f0f0f0f;
		const uint64_t h01 = 0x0101010101010101;

		const uint64_t* const next = (const uint64_t*)m_data;
		size_t i = start;

		// Sum manully until 64 bit aligned
		for(; (i < end) && ((i * m_width) % 64 != 0); i++)
			sum += Get(i);

		if (m_width == 1) {
			const size_t chunkvals = 64;
			for (; i + chunkvals <= end; i += chunkvals) {
				uint64_t a = next[i / chunkvals];

				a -= (a >> 1) & m1;
				a = (a & m2) + ((a >> 2) & m2);
				a = (a + (a >> 4)) & m4;
				a = (a * h01) >> 56;

				// Could use intrinsic instead:
				// a = __builtin_popcountll(a); // gcc intrinsic

				sum += a;
			}
		}
		else if (m_width == 2) {
			const size_t chunkvals = 32;
			for (; i + chunkvals <= end; i += chunkvals) {
				uint64_t a = next[i / chunkvals];

				a = (a & m2) + ((a >> 2) & m2);
				a = (a + (a >> 4)) & m4;
				a = (a * h01) >> 56;

				sum += a;
			}
		}
		else if (m_width == 4) {
			const size_t chunkvals = 16;
			for (; i + chunkvals <= end; i += chunkvals) {
				uint64_t a = next[i / chunkvals];

				a = (a & m4) + ((a >> 4) & m4);
				a = (a * h01) >> 56;

				sum += a;
			}
		}

		// Sum remainding elements 
		for(; i < end; i++)
			sum += Get(i);
	}

	return sum;
}


void Array::FindAllHamming(Array& result, uint64_t value, size_t maxdist, size_t offset) const {
	// Only implemented for 64bit values
	if (m_width != 64) {
		assert(false);
		return;
	}

	const uint64_t* p = (const uint64_t*)m_data;
	const uint64_t* const e = (const uint64_t*)m_data + m_len;

	// static values needed for population count
	const uint64_t m1  = 0x5555555555555555;
	const uint64_t m2  = 0x3333333333333333;
	const uint64_t m4  = 0x0f0f0f0f0f0f0f0f;
	const uint64_t h01 = 0x0101010101010101;

	while (p < e) {
		uint64_t x = *p ^ value;

		// population count
#if defined(WIN32) && defined(SSE42)
		x = _mm_popcnt_u64(x); // msvc sse4.2 intrinsic
#elif defined(GCC)
		x = __builtin_popcountll(x); // gcc intrinsic
#else
		x -= (x >> 1) & m1;
		x = (x & m2) + ((x >> 2) & m2);
		x = (x + (x >> 4)) & m4;
		x = (x * h01)>>56;
#endif

		if (x < maxdist) {
			const size_t pos = p - (const uint64_t*)m_data;
			result.AddPositiveLocal(offset + pos);
		}

		++p;
	}
}

size_t Array::CalcByteLen(size_t count, size_t width) const {
	const size_t bits = (count * width);
	size_t bytes = (bits / 8) + 8; // add room for 8 byte header
	if (bits & 0x7) ++bytes;       // include partial bytes
	return bytes;
}

size_t Array::CalcItemCount(size_t bytes, size_t width) const {
	if (width == 0) return (size_t)-1; // zero width gives infinite space

	const size_t bytes_data = bytes - 8; // ignore 8 byte header
	const size_t total_bits = bytes_data * 8;
	return total_bits / width;
}

bool Array::CopyOnWrite() {
	if (!m_alloc.IsReadOnly(m_ref)) return true;

	// Calculate size in bytes (plus a bit of extra room for expansion)
	size_t len = CalcByteLen(m_len, m_width);
	const size_t rest = (~len & 0x7)+1;
	if (rest < 8) len += rest; // 64bit blocks
	const size_t new_len = len + 64;

	// Create new copy of array
	const MemRef mref = m_alloc.Alloc(new_len);
	if (!mref.pointer) return false;
	memcpy(mref.pointer, m_data-8, len);
	
	// Update internal data
	m_ref = mref.ref;
	m_data = (unsigned char*)mref.pointer + 8;
	m_capacity = CalcItemCount(new_len, m_width);

	// Update capacity in header
	set_header_capacity(new_len); // uses m_data to find header, so m_data must be initialized correctly first

	// Update ref in parent
	if (m_parent) m_parent->Set(m_parentNdx, mref.ref);

	return true;
}

bool Array::Alloc(size_t count, size_t width) {
	if (count > m_capacity || width != m_width) {
		const size_t len      = CalcByteLen(count, width);              // bytes needed
		const size_t capacity = m_capacity ? get_header_capacity() : 0; // bytes currently available
		size_t new_capacity   = capacity;

		if (len > capacity) {
			// Double to avoid too many reallocs
			new_capacity = capacity ? capacity * 2 : 128;
			if (new_capacity < len) {
				const size_t rest = (~len & 0x7)+1;
				new_capacity = len;
				if (rest < 8) new_capacity += rest; // 64bit align
			}

			// Allocate the space
			MemRef mref;
			if (m_data) mref = m_alloc.ReAlloc(m_ref, m_data-8, new_capacity);
			else mref = m_alloc.Alloc(new_capacity);

			if (!mref.pointer) return false;

			const bool isFirst = (capacity == 0);
			m_ref = mref.ref;
			m_data = (unsigned char*)mref.pointer + 8;

			// Create header
			if (isFirst) {
				set_header_isnode(m_isNode);
				set_header_hasrefs(m_hasRefs);
				set_header_wtype(GetWidthType());
				set_header_width(width);
			}
			set_header_capacity(new_capacity);

			// Update ref in parent
			if (m_parent) m_parent->Set(m_parentNdx, mref.ref); //TODO: ref
		}

		m_capacity = CalcItemCount(new_capacity, width);
		set_header_width(width);
	}

	// Update header
	set_header_len(count);

	return true;
}

void Array::SetWidth(size_t width) {
	if (width == 0) {
		m_getter = &Array::Get_0b;
		m_setter = &Array::Set_0b;
	}
	else if (width == 1) {
		m_getter = &Array::Get_1b;
		m_setter = &Array::Set_1b;
	}
	else if (width == 2) {
		m_getter = &Array::Get_2b;
		m_setter = &Array::Set_2b;
	}
	else if (width == 4) {
		m_getter = &Array::Get_4b;
		m_setter = &Array::Set_4b;
	}
	else if (width == 8) {
		m_getter = &Array::Get_8b;
		m_setter = &Array::Set_8b;
	}
	else if (width == 16) {
		m_getter = &Array::Get_16b;
		m_setter = &Array::Set_16b;
	}
	else if (width == 32) {
		m_getter = &Array::Get_32b;
		m_setter = &Array::Set_32b;
	}
	else if (width == 64) {
		m_getter = &Array::Get_64b;
		m_setter = &Array::Set_64b;
	}
	else {
		assert(false);
	}
//	printf("%d ", width);
	SetBounds(width);
	m_width = width;
}

int64_t Array::Get_0b(size_t) const {
	return 0;
}

int64_t Array::Get_1b(size_t ndx) const {
	const size_t offset = ndx >> 3;
	return (m_data[offset] >> (ndx & 7)) & 0x01;
}

int64_t Array::Get_2b(size_t ndx) const {
	const size_t offset = ndx >> 2;
	return (m_data[offset] >> ((ndx & 3) << 1)) & 0x03;
}

int64_t Array::Get_4b(size_t ndx) const {
	const size_t offset = ndx >> 1;
	return (m_data[offset] >> ((ndx & 1) << 2)) & 0x0F;
}

int64_t Array::Get_8b(size_t ndx) const {
	return *((const signed char*)(m_data + ndx));
}

int64_t Array::Get_16b(size_t ndx) const {
	const size_t offset = ndx * 2;
	return *(const int16_t*)(m_data + offset);
}

int64_t Array::Get_32b(size_t ndx) const {
	const size_t offset = ndx * 4;
	return *(const int32_t*)(m_data + offset);
}

int64_t Array::Get_64b(size_t ndx) const {
	const size_t offset = ndx * 8;
	return *(const int64_t*)(m_data + offset);
}

void Array::Set_0b(size_t, int64_t) {
}

void Array::Set_1b(size_t ndx, int64_t value) {
	const size_t offset = ndx >> 3;
	ndx &= 7;

	uint8_t* p = &m_data[offset];
	*p = (*p &~ (1 << ndx)) | (((uint8_t)value & 1) << ndx);
}

void Array::Set_2b(size_t ndx, int64_t value) {
	const size_t offset = ndx >> 2;
	const uint8_t n = (ndx & 3) << 1;

	uint8_t* p = &m_data[offset];
	*p = (*p &~ (0x03 << n)) | (((uint8_t)value & 0x03) << n);
}

void Array::Set_4b(size_t ndx, int64_t value) {
	const size_t offset = ndx >> 1;
	const uint8_t n = (ndx & 1) << 2;

	uint8_t* p = &m_data[offset];
	*p = (*p &~ (0x0F << n)) | (((uint8_t)value & 0x0F) << n);
}

void Array::Set_8b(size_t ndx, int64_t value) {
	*((char*)m_data + ndx) = (char)value;
}

void Array::Set_16b(size_t ndx, int64_t value) {
	const size_t offset = ndx * 2;
	*(int16_t*)(m_data + offset) = (int16_t)value;
}

void Array::Set_32b(size_t ndx, int64_t value) {
	const size_t offset = ndx * 4;
	*(int32_t*)(m_data + offset) = (int32_t)value;
}

void Array::Set_64b(size_t ndx, int64_t value) {
	const size_t offset = ndx * 8;
	*(int64_t*)(m_data + offset) = value;
}

void Array::Sort() {
	DoSort(0, m_len-1);
}

void Array::DoSort(size_t lo, size_t hi) {
	// Quicksort based on
	// http://www.inf.fh-flensburg.de/lang/algorithmen/sortieren/quick/quicken.htm
	int i = (int)lo;
	int j = (int)hi;

	// comparison element x
	const size_t ndx = (lo + hi)/2;
	const int64_t x = (size_t)Get(ndx);

	// partition
	do {
		while (Get(i) < x) i++;
		while (Get(j) > x) j--;
		if (i <= j) {
			const int64_t h = Get(i);
			Set(i, Get(j));
			Set(j, h);
			i++; j--;
		}
	} while (i <= j);

	//  recursion
	if ((int)lo < j) DoSort(lo, j);
	if (i < (int)hi) DoSort(i, hi);
}

#ifdef _DEBUG
#include "stdio.h"

bool Array::Compare(const Array& c) const {
	if (c.Size() != Size()) return false;

	for (size_t i = 0; i < Size(); ++i) {
		if (Get(i) != c.Get(i)) return false;
	}

	return true;
}

void Array::Print() const {
	printf("%zx: (%zu) ", GetRef(), Size());
	for (size_t i = 0; i < Size(); ++i) {
		if (i) printf(", ");
		printf("%d", (int)Get(i));
	}
	printf("\n");
}

void Array::Verify() const {
	assert(m_width == 0 || m_width == 1 || m_width == 2 || m_width == 4 || m_width == 8 || m_width == 16 || m_width == 32 || m_width == 64);
}

void Array::ToDot(FILE* f, bool) const{
	const size_t ref = GetRef();

	fprintf(f, "n%zx [label=\"", ref);

	//if (!horizontal) fprintf(f, "{");
	for (size_t i = 0; i < m_len; ++i) {
		if (i > 0) fprintf(f, " | ");

		if (m_hasRefs) fprintf(f, "<%zu>",i);
		else fprintf(f, "%lld", Get(i));
	}
	//if (!horizontal) fprintf(f, "}");
	
	fprintf(f, "\"];\n");

	if (m_hasRefs) {
		for (size_t i = 0; i < m_len; ++i) {
			fprintf(f, "n%zx:%zu -> n%lld\n", ref, i, Get(i));
		}
	}
	fprintf(f, "\n");
}

MemStats Array::Stats() const {
	return MemStats(m_capacity, CalcByteLen(m_len, m_width), 1);
}

#endif //_DEBUG
