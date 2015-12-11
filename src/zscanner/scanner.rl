/*  Copyright (C) 2011 CZ.NIC, z.s.p.o. <knot-dns@labs.nic.cz>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <arpa/inet.h>
#include <config.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libgen.h>
#include <math.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "zscanner/scanner.h"
#include "zscanner/functions.h"
#include "libknot/descriptor.h"

/*! \brief Shorthand for setting warning data. */
#define WARN(err_code) { s->error.code = err_code; }
/*! \brief Shorthand for setting error data. */
#define ERR(err_code) { WARN(err_code); s->error.fatal = true; }
/*! \brief Shorthand for error reset. */
#define NOERR { WARN(ZS_OK); s->error.fatal = false; }

/*!
 * \brief Writes record type number to r_data.
 *
 * \param type		Type number.
 * \param rdata_tail	Position where to write type number to.
 */
static inline void type_num(const uint16_t type, uint8_t **rdata_tail)
{
	*((uint16_t *)*rdata_tail) = htons(type);
	*rdata_tail += 2;
}

/*!
 * \brief Sets bit to bitmap window.
 *
 * \param type		Type number.
 * \param s		Scanner context.
 */
static inline void window_add_bit(const uint16_t type, zs_scanner_t *s) {
	uint8_t win      = type / 256;
	uint8_t bit_pos  = type % 256;
	uint8_t byte_pos = bit_pos / 8;

	((s->windows[win]).bitmap)[byte_pos] |= 128 >> (bit_pos % 8);

	if ((s->windows[win]).length < byte_pos + 1) {
		(s->windows[win]).length = byte_pos + 1;
	}

	if (s->last_window < win) {
		s->last_window = win;
	}
}

// Include scanner file (in Ragel).
%%{
	machine zone_scanner;

	include "scanner_body.rl";

	write data;
}%%

__attribute__((visibility("default")))
int zs_init(
	zs_scanner_t *s,
	const char *origin,
	const uint16_t rclass,
	const uint32_t ttl)
{
	if (s == NULL) {
		return -1;
	}

	memset(s, 0, sizeof(*s));

	// Nonzero initial scanner state.
	s->cs = %%{ write start; }%%;

	// Reset the file descriptor.
	s->file.descriptor = -1;

	// Use the root zone as origin if not specified.
	if (origin == NULL || strlen(origin) == 0) {
		origin = ".";
	}
	size_t origin_len = strlen(origin);

	// Prepare a zone settings header.
	const char *format;
	if (origin[origin_len - 1] != '.') {
		format = "$ORIGIN %s.\n";
	} else {
		format = "$ORIGIN %s\n";
	}

	char settings[1024];
	int ret = snprintf(settings, sizeof(settings), format, origin);
	if (ret <= 0 || ret >= sizeof(settings)) {
		ERR(ZS_ENOMEM);
		return -1;
	}

	// Parse the settings to set up the scanner origin.
	if (zs_set_input_string(s, settings, ret) != 0 ||
	    zs_parse_all(s) != 0) {
		return -1;
	}

	// Set scanner defaults.
	s->path = strdup(".");
	if (s->path == NULL) {
		ERR(ZS_ENOMEM);
		return -1;
	}
	s->default_class = rclass;
	s->default_ttl = ttl;
	s->line_counter = 1;

	s->state = ZS_STATE_EOF;
	s->process.automatic = false;

	return 0;
}

static void file_deinit(
	zs_scanner_t *s)
{
	if (s->file.descriptor == -1) {
		return;
	}

	// Clean-up the opened file.
	munmap((void *)s->input.start, s->input.end - s->input.start);
	close(s->file.descriptor);
	free(s->file.name);
	s->file.descriptor = -1;
}

__attribute__((visibility("default")))
void zs_deinit(
	zs_scanner_t *s)
{
	if (s == NULL) {
		return;
	}

	file_deinit(s);
	free(s->path);
}

__attribute__((visibility("default")))
int zs_set_input_string(
	zs_scanner_t *s,
	const char *input,
	size_t size)
{
	if (s == NULL) {
		return -1;
	}

	if (input == NULL) {
		ERR(ZS_EINVAL);
		return -1;
	}

	// Deinit possibly opened file.
	file_deinit(s);

	// Set the scanner input limits.
	s->input.start   = input;
	s->input.current = input;
	s->input.end     = input + size;
	s->input.eof     = false;

	return 0;
}

__attribute__((visibility("default")))
int zs_set_input_file(
	zs_scanner_t *s,
	const char *file_name)
{
	if (s == NULL) {
		return -1;
	}

	if (file_name == NULL) {
		ERR(ZS_EINVAL);
		return -1;
	}

	// Deinit possibly opened file.
	file_deinit(s);

	// Try to open the file.
	s->file.descriptor = open(file_name, O_RDONLY);
	if (s->file.descriptor == -1) {
		ERR(ZS_FILE_OPEN);
		return -1;
	}

	// Check for regular file input.
	struct stat file_stat;
	if (fstat(s->file.descriptor, &file_stat) == -1 ||
	    !S_ISREG(file_stat.st_mode)) {
		ERR(ZS_FILE_INVALID);
		goto file_error;
	}

	char *start = NULL;

	// Check for empty file (cannot mmap).
	if (file_stat.st_size > 0) {
		// Map the file to the memory.
		start = mmap(0, file_stat.st_size, PROT_READ, MAP_SHARED,
		             s->file.descriptor, 0);
		if (start == MAP_FAILED) {
			ERR(ZS_FILE_MMAP);
			goto file_error;
		}

		// Try to set the mapped memory advise to sequential.
		(void)madvise(start, file_stat.st_size, MADV_SEQUENTIAL);

		s->input.eof = false;
	} else {
		s->input.eof = true;
	}

	// Get absolute path of the zone file.
	char *full_name = realpath(file_name, NULL);
	if (full_name != NULL) {
		free(s->path);
		s->path = strdup(dirname(full_name));
		free(full_name);
		if (s->path == NULL) {
			ERR(ZS_ENOMEM);
			goto file_error;
		}
	} else {
		ERR(ZS_FILE_PATH);
		goto file_error;
	}

	s->file.name = strdup(file_name);
	if (s->file.name == NULL) {
		ERR(ZS_ENOMEM);
		goto file_error;
	}

	// Set the scanner input limits.
	s->input.start   = start;
	s->input.current = start;
	s->input.end     = start + file_stat.st_size;

	return 0;
file_error:
	close(s->file.descriptor);
	s->file.descriptor = -1;
	free(s->file.name);

	return -1;
}

__attribute__((visibility("default")))
int zs_set_processing(
	zs_scanner_t *s,
	void (*process_record)(zs_scanner_t *),
	void (*process_error)(zs_scanner_t *),
	void *data)
{
	if (s == NULL) {
		return -1;
	}

	s->process.record = process_record;
	s->process.error = process_error;
	s->process.data = data;

	return 0;
}

static void parse(
	zs_scanner_t *s)
{
	// Restore scanner input limits (Ragel internals).
	const char *p = s->input.current;
	const char *pe = s->input.end;
	const char *eof = s->input.eof ? pe : NULL;

	// Restore state variables (Ragel internals).
	int cs  = s->cs;
	int top = s->top;
	int stack[RAGEL_STACK_SIZE];
	memcpy(stack, s->stack, sizeof(stack));

	// Auxiliary variables which are used in scanner body.
	struct in_addr  addr4;
	struct in6_addr addr6;
	uint32_t timestamp;
	int16_t  window;
	int      ret;

	// Next 2 variables are for better performance.
	// Restoring r_data pointer to next free space.
	uint8_t *rdata_tail = s->r_data + s->r_data_tail;
	// Initialization of the last r_data byte.
	uint8_t *rdata_stop = s->r_data + MAX_RDATA_LENGTH - 1;

	// External processing interrupt indicator.
	bool escape = false;

	// Write scanner body (in C).
	%% write exec;

	// Check if the scanner state machine is in an uncovered state.
	if (cs == %%{ write error; }%%) {
		ERR(ZS_UNCOVERED_STATE);
		s->error.counter++;

		// Fill error context data.
		for (s->buffer_length = 0;
		     ((p + s->buffer_length) < pe) &&
		     (s->buffer_length < sizeof(s->buffer) - 1);
		     s->buffer_length++)
		{
			// Only rest of the current line.
			if (*(p + s->buffer_length) == '\n') {
				break;
			}
			s->buffer[s->buffer_length] = *(p + s->buffer_length);
		}

		// Ending string in buffer.
		s->buffer[s->buffer_length++] = 0;

		s->state = ZS_STATE_ERROR;

		// Execute the error callback.
		if (s->process.automatic && s->process.error != NULL) {
			s->process.error(s);
		}

		return;
	}

	// Check unclosed multiline record.
	if (s->input.eof && s->multiline) {
		ERR(ZS_UNCLOSED_MULTILINE);
		s->error.counter++;

		s->state = ZS_STATE_ERROR;

		// Execute the error callback.
		if (s->process.automatic && s->process.error != NULL) {
			s->process.error(s);
		}

		return;
	}

	// Storing scanner states.
	s->cs  = cs;
	s->top = top;
	memcpy(s->stack, stack, sizeof(stack));

	// Store the current parser position.
	s->input.current = p;

	// Storing r_data pointer.
	s->r_data_tail = rdata_tail - s->r_data;
}

__attribute__((visibility("default")))
int zs_parse_record(
	zs_scanner_t *s)
{
	if (s == NULL) {
		return -1;
	}

	// Stop parsing if stop or after the final parsing.
	if (s->state == ZS_STATE_STOP || s->input.eof) {
		return -1;
	}

	// Check for the end of the input.
	if (s->input.current != s->input.end) {
		// Parse the next item.
		parse(s);
		return 0;
	} else if (s->state != ZS_STATE_EOF) {
		// Indicate end of the input.
		s->state = ZS_STATE_EOF;
		return 0;
	} else {
		// Parse the final block.
		if (zs_set_input_string(s, "\n", 1) != 0) {
			return -1;
		}
		s->input.eof = true;
		parse(s);
		return 0;
	}
}

__attribute__((visibility("default")))
int zs_parse_all(
	zs_scanner_t *s)
{
	if (s == NULL) {
		return -1;
	}

	s->process.automatic = true;

	// Parse input block.
	parse(s);

	// Parse trailing newline-char block if not stop.
	if (s->state != ZS_STATE_STOP) {
		if (zs_set_input_string(s, "\n", 1) != 0) {
			return -1;
		}
		s->input.eof = true;
		parse(s);
	}

	// Check if any errors has occured.
	if (s->error.counter > 0) {
		return -1;
	}

	return 0;
}
