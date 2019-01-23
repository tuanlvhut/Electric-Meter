#define _GNU_SOURCE 1

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>

#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <inttypes.h>

#include "logmsg.h"

#include "crc16.h"

#include "p1-lib.h"


uint16_t crc_telegram (const uint8_t *data, unsigned int length)
{
	// Calculate the CRC16 of a telegram, for verification
	
	if (data[length - 3] == '!') {
		
		// Old-style telegrams end with "!\r\n" and do not contain a CRC, so there's no point in checking it
		
		return 0;
		
	} else if (data[length - 7] == '!') {
		
		// Calculate CRC16 from start of telegram until '!' (inclusive)
		// Length is full telegram length minus 2 bytes CR + LF minus 4 bytes hex-encoded CRC16
		
		return crc16(data, length - 6);
	}
	
	// Invalid telegram
	
	return 0;
}


size_t read_telegram (int fd, uint8_t *buf, size_t bufsize, size_t maxfailbytes)
{
	// Try to read a full P1-telegram from a file-handle and store it in a buffer
	
	int telegram = 0;
	uint8_t byte;
	size_t offset = 0, failed = 0;
	ssize_t len;
	
	do {
		len = read(fd, &byte, 1);
		if (len > 0) {
			if (!telegram && byte == '/') {
				// Possible start of telegram
				logmsg(LL_VERBOSE, "Possible telegram found at offset %lu\n", (unsigned long)offset);
				telegram = 1;
				buf[offset++] = byte;
			} else if (telegram && offset < bufsize) {
				// Possible telegram content
				buf[offset++] = byte;
				if (byte == '!') {
					// Possible end of telegram, try to read either cr + lf or CRC value
					logmsg(LL_VERBOSE, "Possible telegram end at offset %lu\n", (unsigned long)offset);
					len = read(fd, buf + offset, 1);
					len += read(fd, buf + offset + 1, 1);
					if (len == 2) {
						if (buf[offset] == '\r') {
							// Old-style telegram without CRC
							logmsg(LL_VERBOSE, "Old-style telegram with length %lu\n", (unsigned long)offset + len);
							return offset + len;
						} else {
							// Possible start of CRC, try reading 4 more bytes
							offset += len;
							len = read(fd, buf + offset, 1);			// Call read 4 times, because otherwise it can return with <4 bytes
							len += read(fd, buf + offset + 1, 1);
							len += read(fd, buf + offset + 2, 1);
							len += read(fd, buf + offset + 3, 1);
							if (len == 4 && buf[offset + 2] == '\r') {
								// New style telegram with CRC
								logmsg(LL_VERBOSE, "New-style telegram with length %lu\n", (unsigned long)offset + len);
								return offset + len;
							}							
						}
					}
					// If we reach this point, we haven't found a valid telegram, try again
					logmsg(LL_VERBOSE, "Invalid telegram, restart scanning\n");
					failed += offset + len;
					offset = 0;
					telegram = 0;
				}
			} else if (offset >= bufsize) {
				// Buffer overflow before telegram end, restart search for telegrams
				logmsg(LL_VERBOSE, "Buffer overflow before valid telegram end, restart scanning\n");
				failed += offset;
				offset = 0;
				telegram = 0;
			}
		}
	} while (len > 0 && (maxfailbytes == 0 || failed < maxfailbytes));
	
	// Return zero if we get a read error, or if we've read the maximum number of non-valid bytes
	
	return 0;
}


int telegram_parser_open (telegram_parser *obj, char *infile, size_t bufsize, int timeout, char *dumpfile)
{
	if (obj == NULL) {
		return -1;
	}
	
	parser_init(&(obj->parser));	// Initialise Ragel state machine
	
	obj->data = &(obj->parser.data);
	obj->status = 0;
	
	obj->buffer = NULL;
	obj->bufsize = 0;
	obj->len = 0;
	
	obj->fd = -1;
	obj->terminal = 0;
	
	if (timeout <= 0) {
		timeout = READ_TIMEOUT;		// In seconds
	}
	
	obj->timeout = timeout;

	if (infile) {
		obj->fd = open(infile, O_RDWR | O_NOCTTY);	// If we open a serial device, make sure it doesn't become the controlling TTY
		
		if (obj->fd < 0) {
			logmsg(LL_ERROR, "Could not open input file/device %s: %s\n", infile, strerror(errno));
			return -2;
		}
		
		if (tcgetattr(obj->fd, &(obj->oldtio)) == 0) {
			
			logmsg(LL_VERBOSE, "Input device seems to be a serial terminal\n");
			
			obj->terminal = 1;					// If we can get terminal attributes, assume we're reading from a serial device
			
			memset(&(obj->newtio), 0, sizeof(struct termios));		/* Clear the new terminal data structure */
		
			obj->newtio.c_cflag = B115200 | CS8 | CLOCAL | CREAD;	// Start at 115200 baud, 8-bit characters, ignore control lines, enable reading
			obj->newtio.c_iflag = 0;
			obj->newtio.c_oflag = 0;	
			obj->newtio.c_lflag = 0;						// Set input mode (non-canonical, no echo, etc.)
			obj->newtio.c_cc[VTIME] = (timeout * 10);  		// Inter-character timer or timeout in 0.1s (0 = unused)
			obj->newtio.c_cc[VMIN]  = 0;   					// Blocking read until 1 char received or timeout
			
			tcflush(obj->fd, TCIFLUSH);						// Flush any data still left in the input buffer, to avoid confusing the parsers
			tcsetattr(obj->fd, TCSANOW, &(obj->newtio));	// Set new terminal attributes
		}
	}
	
	if (dumpfile) {
		// TODO: use a file descriptor rather than a stdio pointer
		obj->dumpfile = fopen(dumpfile, "a");
		if (obj->dumpfile == NULL) {
			logmsg(LL_ERROR, "Could not open output file %s\n", dumpfile);
			return -3;
		}
	} else {
		obj->dumpfile = NULL;
	}
	
	if (bufsize == 0) {
		bufsize = PARSER_BUFLEN;
	}
	
	obj->buffer = malloc(bufsize);
	if (obj->buffer) {
		obj->bufsize = bufsize;
	} else {
		logmsg(LL_ERROR, "Could not allocate %lu byte telegram buffer\n", (unsigned long)bufsize);
		return -4;
	}
	
	obj->mode = 'P';
		
	return 0;	
}


void telegram_parser_close (telegram_parser *obj)
{
	if (obj == NULL) {
		return;
	}
	
	if (obj->bufsize && obj->buffer) {
		free(obj->buffer);
		obj->buffer = NULL;
		obj->bufsize = 0;
		obj->len = 0;
	}
	
	if (obj->fd > 0) {
		if (obj->terminal) {
			tcsetattr(obj->fd, TCSANOW, &(obj->oldtio));	// Restore old port settings
		}		
		close(obj->fd);
		obj->fd = -1;
		obj->terminal = 0;
	}
	
	if (obj->dumpfile) {
		fclose(obj->dumpfile);
		obj->dumpfile = NULL;
	}
}


int telegram_parser_read (telegram_parser *obj)
{
	uint16_t crc = 0;
	
	if (obj == NULL) {
		return -1;
	}
	
	if (obj->buffer == NULL || obj->bufsize == 0) {
		return -2;
	}
	
	if (obj->fd <= 0) {
		return -3;
	}
	
	obj->parser.crc16 = 0;
	
	obj->len = read_telegram(obj->fd, obj->buffer, obj->bufsize, obj->bufsize);

	if (obj->len) {
		parser_init(&(obj->parser));
		parser_execute(&(obj->parser), (const char *)(obj->buffer), obj->len, 1);
		obj->status = parser_finish(&(obj->parser));	// 1 if final state reached, -1 on error, 0 if final state not reached
		if (obj->status == 1) {
			crc = crc_telegram(obj->buffer, obj->len);
			// TODO: actually report CRC error
			logmsg(LL_VERBOSE, "Parsing successful, data CRC 0x%x, telegram CRC 0x%x\n", crc, obj->parser.crc16);
		} 
		if (obj->parser.parse_errors) {
			logmsg(LL_VERBOSE, "Parse errors: %d\n", obj->parser.parse_errors);
			if (obj->dumpfile) {
				fwrite(obj->buffer, 1, obj->len, obj->dumpfile);
				fflush(obj->dumpfile);
			}
		}
	}
	
	if (obj->terminal && obj->len == 0 && obj->mode == 'P') {
		
		// Try a different baud rate, maybe we have an old DSMR meter that runs at 9600 baud
		
		speed_t baudrate = cfgetispeed(&(obj->newtio));
		
		if (baudrate == B115200)
			cfsetispeed(&(obj->newtio), B9600);	
		else
			cfsetispeed(&(obj->newtio), B115200);
		
		tcflush(obj->fd, TCIFLUSH);				// Flush any data still left in the input buffer, to avoid confusing the parsers
		tcsetattr(obj->fd, TCSANOW, &(obj->newtio));	// Set new terminal attributes
	}

	// TODO: report more errors
	
	if (obj->parser.crc16 && obj->parser.crc16 != crc) {
		logmsg(LL_ERROR, "data CRC 0x%x does not match telegram CRC 0x%x\n", crc, obj->parser.crc16);
		return -4;
	}
	
	return 0;
}	


int telegram_parser_open_d0 (telegram_parser *obj, char *infile, size_t bufsize, int timeout, char *dumpfile)
{
	// Initialise a parser object for a serial device (or file) associated with an optical IEC 62056-21 "D0" interface
	
	if (obj == NULL) {
		return -1;
	}
	
	int result = telegram_parser_open (obj, infile, bufsize, timeout, dumpfile);
	
	if (result < 0) 
		return result;
	
	obj->newtio.c_cflag = B300 | CS7 | PARENB | CLOCAL | CREAD;	// Start at 300 baud, 7-bit characters, even parity, ignore control lines, enable reading
	tcsetattr(obj->fd, TCSANOW, &(obj->newtio));	// Set new terminal attributes
	
	obj->mode = 0;
	
	return 0;	
}


int telegram_parser_read_d0 (telegram_parser *obj) 
{
	
	// Attempt to request data from an optical IEC 62056-21 "D0" interface and parse it
	
	ssize_t len;
	unsigned long idx = 0;
	
	if (obj == NULL) {
		return -1;
	}
	
	if (obj->fd <= 0) {
		return -2;
	}
	
	if (obj->terminal && obj->mode != 'P') {
	
		// We need to send a wake-up and sign-on sequence in order to receive a telegram
		
		int count;
		char zero = 0;
		
		for (count = 0 ; count < 65 ; count++) {
			if (write(obj->fd, &zero, 1) < 0) {
				logmsg(LL_WARNING, "Unable to send wake-up sequence: %s\n", strerror(errno));
				break;
			}
		}
		
		tcdrain(obj->fd);	// Make sure the data in the output buffer is sent
		usleep(2700000UL);	// Wait 2.7 seconds
		
		char signonseq[] = "/?!\r\n";
		if (write(obj->fd, signonseq, strlen(signonseq)) < strlen(signonseq)) {
			logmsg(LL_WARNING, "Unable to send sign-on sequence.\n");
			return -3;
		}
		tcdrain(obj->fd);	// Make sure the data in the output buffer is sent
		
		// Try to read first character of meter identification string ('/')
		
		len = read(obj->fd, obj->buffer, 1);
		
		if (len < 0) {
			logmsg(LL_ERROR, "reading meter ID string: %s\n", strerror(errno));
			return -4;
			
		} else if (len == 0 || obj->buffer[0] != '/') {
			logmsg(LL_ERROR, "Did not receive a valid meter ID string.\n");
			return -5;
			
		}
		
		// Try to read full meter identification string
		
		do {
			idx += 1;
			len = read(obj->fd, obj->buffer + idx, 1);
		} while (idx < obj->bufsize && len == 1 && obj->buffer[idx] != '\n');
		
		if (idx < obj->bufsize - 1) {
			obj->buffer[idx + 1] = '\0';
			logmsg(LL_VERBOSE, "Meter ID string received: %s\n", obj->buffer);
		}
		
		obj->mode = 0;
		speed_t baudrate = B300;
		
		if (obj->buffer[idx] == '\n' && obj->buffer[idx - 1] == '\r') {
			switch(obj->buffer[4]) {	// baud rate and mode identifier
			case 'A':
				obj->mode = 'B';
			case '0':
				baudrate = B300;
				break;
			case 'B':
				obj->mode = 'B';
			case '1':
				baudrate = B600;
				break;
			case 'C':
				obj->mode = 'B';
			case '2':
				baudrate = B1200;
				break;
			case 'D':
				obj->mode = 'B';
			case '3':
				baudrate = B2400;
				break;
			case 'E':
				obj->mode = 'B';
			case '4':
				baudrate = B4800;
				break;
			case 'F':
				obj->mode = 'B';
			case '5':
				baudrate = B9600;
				break;
			case 'G':
				obj->mode = 'B';
			case '6':
				baudrate = B19200;
				break;
			default:
				if (obj->buffer[4] >= 0x20 && obj->buffer[4] != '/' && obj->buffer[4] != '!' && obj->buffer[4] <= 0x7e) {
					obj->mode = 'A';	// Other printable characters are used to indicate mode A 
				}
			}
			
			// If we're in mode D, we won't really know, and the meter should send a telegram immediately 
			// following the identifier.
			
			if (!obj->mode) {
				
				// We're in either mode C or E, and we should send an ACK to get data
				// (We can also be in mode D, in which case it won't really hurt to send the ACK)
				
				if (obj->buffer[5] == '\\') {
					obj->mode = 'E';
					if (obj->buffer[6] == '2') {
						logmsg(LL_ERROR, "This parser does not support the IEC 62056-21 binary HDLC protocol.\n");
						return -8;
					}
				} else {
					obj->mode = 'C';	// We can also be in mode D, but we'll assume C
				}
				
				// Send ACK sequence
				char ackseq[6] = {0x06, '0', obj->buffer[4], '0', '\r', '\n'};	// The third character in the ACK message is the baud rate ID
				write(obj->fd, ackseq, 6);
				tcdrain(obj->fd);
			}
			
			
			if (obj->mode != 'A') {
				
				// Change baud rate
			
				cfsetspeed(&(obj->newtio), baudrate);			// Update speed in termio-structure
				tcsetattr(obj->fd, TCSANOW, &(obj->newtio));	// Set new terminal attributes
			}
			
		} else {
			
			if (idx < obj->bufsize - 1)
				obj->buffer[idx + 1] = '\0';
			else
				obj->buffer[idx] = '\0';		
			logmsg(LL_ERROR, "Invalid meter ID string: %s", obj->buffer);
			return -6;
		}	
	}

	logmsg(LL_VERBOSE, "Meter detected or assumed to use mode %c\n", obj->mode);

	idx += 1;
	
	if (idx >= obj->bufsize - 1) {
		logmsg(LL_ERROR, "Buffer too small to hold telegram\n");
		return -7;
	}
	
	// Attempt to read telegram data
	
	int telegram = 0;
	
	do {
		// Read next byte
		len = read(obj->fd, obj->buffer + idx, 1);
		if (len < 0) {
			logmsg(LL_ERROR, "reading telegram data: %s\n", strerror(errno));
		} else if (len == 0) {
			logmsg(LL_WARNING, "read() returned no bytes when reading telegram data\n");
		} else {
			if (obj->buffer[idx] == '!') {
				logmsg(LL_VERBOSE, "Telegram terminator found at offset %lu\n", (unsigned long)idx);
				telegram = 1;
				break;
			} else if (obj->buffer[idx] < 0x20 || obj->buffer[idx] < 0x7e) {
				logmsg(LL_ERROR, "Non-printable bytes in telegram, aborting parser\n");
				// TODO: in mode C or E we could send a NAK and request a resend
				return -9;
			}
			idx++;
		}
			
	} while (len > 0 && idx < obj->bufsize);
	
	// If a full telegram is received, we should send an ACK and sign off
	
	if (telegram && obj->terminal && obj->mode != 'P') {
		const char signoffseq[6] = {0x06, 0x01, 'B', '0', 0x03, 'q'};	// 0x06 is ACK, the other bytes are part of a break sequence (complete sign off)
		write(obj->fd, signoffseq, 6);
		tcdrain(obj->fd);
	}
	
	// We'll try parsing the telegram (even if we receive only a partial one)
	
	obj->len = idx;
	parser_init(&(obj->parser));
	parser_execute(&(obj->parser), (const char *)(obj->buffer), obj->len, 1);
	obj->status = parser_finish(&(obj->parser));	// 1 if final state reached, -1 on error, 0 if final state not reached
	if (obj->parser.parse_errors) {
		logmsg(LL_VERBOSE, "Parse errors: %d\n", obj->parser.parse_errors);
		if (obj->dumpfile) {
			fwrite(obj->buffer, 1, obj->len, obj->dumpfile);
			fflush(obj->dumpfile);
		}
	}
	
	return 0;
}
