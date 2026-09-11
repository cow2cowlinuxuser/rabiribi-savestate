#include <string.h>

#include "host.h"
#include "host_pe_data.h"

extern const unsigned char pe_data_bin[];
extern const unsigned char pe_data_bin_end[];

unsigned host_pe_copy(void *dst, unsigned va, unsigned n)
{
	unsigned off;
	unsigned size;

	if (!dst || !n)
		return 0;
	if (va < HOST_PE_DATA_VA)
		return 0;
	off = va - HOST_PE_DATA_VA;
	size = (unsigned)(pe_data_bin_end - pe_data_bin);
	if (off >= size || off + n > size)
		return 0;
	if (off >= HOST_PE_DATA_SIZE || off + n > HOST_PE_DATA_SIZE)
		return 0;
	memcpy(dst, pe_data_bin + off, n);
	return n;
}
