/**@file 2c.c
 * @brief Convert the Abstract Syntax Tree generated by mpc for the DBC file
 * into some C code which can encode/decode signals.
 * @copyright Richard James Howe (2018)
 * @license MIT
 *
 * @todo A data driven version would be better, data should be centralized
 * and the pack/unpack functions should use data structures instead of
 * big functions with switch statements.
 * @todo Signal status; signal should be set to unknown first, or when there
 * is a timeout. A timestamp should also be processed.
 * @todo Add (optional) generation of 'asserts' into code, so pointers can
 * be asserted to be non-NULL, DLCs within range (0-8), ID within ranges (29-bit),
 * and other properties.
 *
 * This file is quite a mess, but that is not going to change, it is also
 * quite short and seems to do the job. A better solution would be to make a
 * template tool, or a macro processor, suited for the task of generating C
 * code. The entire program really should be written in a language like Perl or
 * Python, but I wanted to use the MPC library for something, so here we are. */

#include "2cpp.h"
#include "util.h"
#include <assert.h>
#include <ctype.h>
#include <inttypes.h>
#include <string.h>
#include <time.h>

#define MAX_NAME_LENGTH (512u)

/* The float packing and unpacking is stolen and modified from 
 * <https://beej.us/guide/bgnet/examples/pack2b.c>! 
 * (It's public domain code as far as I know, from Beej's guide to network 
 * programming).
 *
 * The following link provides a calculator you can use to see what
 * bits correspond to a floating point number:
 * <https://www.h-schmidt.net/FloatConverter/IEEE754.html>
 *
 * Special cases:
 *
 * Zero and sign bit set -> Negative Zero
 *
 * All Exponent Bits Set
 * - Mantissa is zero and sign bit is zero ->  Infinity
 * - Mantissa is zero and sign bit is on   -> -Infinity
 * - Mantissa is non-zero -> NaN */

static int message_compare_function(const void *a, const void *b)
{
	assert(a);
	assert(b);
	can_msg_t *ap = *((can_msg_t**)a);
	can_msg_t *bp = *((can_msg_t**)b);
	if (ap->id <  bp->id) return -1;
	if (ap->id == bp->id) return  0;
	if (ap->id >  bp->id) return  1;
	return 0;
}

static int signal_compare_function(const void *a, const void *b)
{
	assert(a);
	assert(b);
	signal_t *ap = *((signal_t**)a);
	signal_t *bp = *((signal_t**)b);
	if (ap->bit_length <  bp->bit_length) return  1;
	if (ap->bit_length == bp->bit_length) return  0;
	if (ap->bit_length >  bp->bit_length) return -1;
	return 0;
}

static char *float_pack = "\
/* pack754() -- pack a floating point number into IEEE-754 format */ \n\
static uint64_t pack754(const double f, const unsigned bits, const unsigned expbits) {\n\
	if (f == 0.0) /* get this special case out of the way */\n\
		return signbit(f) ? (1uLL << (bits - 1)) :  0;\n\
	if (f != f) /* NaN, encoded as Exponent == all-bits-set, Mantissa != 0, Signbit == Do not care */\n\
		return (1uLL << (bits - 1)) - 1uLL;\n\
	if (f == INFINITY) /* +INFINITY encoded as Mantissa == 0, Exponent == all-bits-set */\n\
		return ((1uLL << expbits) - 1uLL) << (bits - expbits - 1);\n\
	if (f == -INFINITY) /* -INFINITY encoded as Mantissa == 0, Exponent == all-bits-set, Signbit == 1 */\n\
		return (1uLL << (bits - 1)) | ((1uLL << expbits) - 1uLL) << (bits - expbits - 1);\n\
\n\
	long long sign = 0;\n\
	double fnorm = f;\n\
	/* check sign and begin normalization */\n\
	if (f < 0) { sign = 1; fnorm = -f; }\n\
\n\
	/* get the normalized form of f and track the exponent */\n\
	int shift = 0;\n\
	while (fnorm >= 2.0) { fnorm /= 2.0; shift++; }\n\
	while (fnorm < 1.0)  { fnorm *= 2.0; shift--; }\n\
	fnorm = fnorm - 1.0;\n\
\n\
	const unsigned significandbits = bits - expbits - 1; // -1 for sign bit\n\
\n\
	/* calculate the binary form (non-float) of the significand data */\n\
	const long long significand = fnorm * (( 1LL << significandbits) + 0.5f);\n\
\n\
	/* get the biased exponent */\n\
	const long long exp = shift + ((1LL << (expbits - 1)) - 1); // shift + bias\n\
\n\
	/* return the final answer */\n\
	return (sign << (bits - 1)) | (exp << (bits - expbits - 1)) | significand;\n\
}\n\
\n\
static inline uint32_t   pack754_32(const float  f)   { return   pack754(f, 32, 8); }\n\
static inline uint64_t   pack754_64(const double f)   { return   pack754(f, 64, 11); }\n\
\n\n";

static char *float_unpack = "\
/* unpack754() -- unpack a floating point number from IEEE-754 format */ \n\
static double unpack754(const uint64_t i, const unsigned bits, const unsigned expbits) {\n\
	if (i == 0) return 0.0;\n\
\n\
	const uint64_t expset = ((1uLL << expbits) - 1uLL) << (bits - expbits - 1);\n\
	if ((i & expset) == expset) { /* NaN or +/-Infinity */\n\
		if (i & ((1uLL << (bits - expbits - 1)) - 1uLL)) /* Non zero Mantissa means NaN */\n\
			return NAN;\n\
		return i & (1uLL << (bits - 1)) ? -INFINITY : INFINITY;\n\
	}\n\
\n\
	/* pull the significand */\n\
	const unsigned significandbits = bits - expbits - 1; /* - 1 for sign bit */\n\
	double result = (i & ((1LL << significandbits) - 1)); /* mask */\n\
	result /= (1LL << significandbits);  /* convert back to float */\n\
	result += 1.0f;                        /* add the one back on */\n\
\n\
	/* deal with the exponent */\n\
	const unsigned bias = (1 << (expbits - 1)) - 1;\n\
	long long shift = ((i >> significandbits) & ((1LL << expbits) - 1)) - bias;\n\
	while (shift > 0) { result *= 2.0; shift--; }\n\
	while (shift < 0) { result /= 2.0; shift++; }\n\
	\n\
	return (i >> (bits - 1)) & 1? -result: result; /* sign it, and return */\n\
}\n\
\n\
static inline float    unpack754_32(uint32_t i) { return unpack754(i, 32, 8); }\n\
static inline double   unpack754_64(uint64_t i) { return unpack754(i, 64, 11); }\n\
\n\n";

static const bool swap_motorola = true;

static const char *determine_unsigned_type(unsigned length)
{
	const char *type = "uint64_t";
	if (length <= 32)
		type = "uint32_t";
	if (length <= 16)
		type = "uint16_t";
	if (length <= 8)
		type = "uint16_t";
	return type;
}

static const char *determine_signed_type(unsigned length)
{
	const char *type = "int64_t";
	if (length <= 32)
		type = "int32_t";
	if (length <= 16)
		type = "int16_t";
	if (length <= 8)
		type = "int8_t";
	return type;
}

static const char *determine_type(unsigned length, bool is_signed)
{
	return is_signed ?
		determine_signed_type(length) :
		determine_unsigned_type(length);
}
static int signal2type(signal_t *sig, FILE *o)
{
	assert(sig);
	assert(o);
	const unsigned length = sig->bit_length;
	const char *type = determine_type(length, sig->is_signed);

	if (length == 0) {
		warning("signal %s has bit length of 0 (fix the dbc file)");
		return -1;
	}

	if (sig->is_floating) {
		if (length != 32 && length != 64) {
			warning("signal %s is floating point number but has length %u (fix the dbc file)", sig->name, length);
			return -1;
		}
		type = length == 64 ? "double" : "float";
	}

	if (sig->comment) {
		fprintf(o, "\t/* %s: %s */\n", sig->name, sig->comment);
		return fprintf(o, "\t/* scaling %.1f, offset %.1f, units %s %s */\n\t%s %s;\n",
				sig->scaling, sig->offset, sig->units[0] ? sig->units : "none",
				sig->is_floating ? ", floating" : "",
				type, sig->name);
	} else {
		return fprintf(o, "\t%s %s; /* scaling %.1f, offset %.1f, units %s %s */\n",
				type, sig->name, sig->scaling, sig->offset, sig->units[0] ? sig->units : "none",
				sig->is_floating ? ", floating" : "");
	}
}

static void make_name(char *newname, size_t maxlen, const char *name, unsigned id)
{
	assert(newname);
	assert(name);
	snprintf(newname, maxlen-1, "can_0x%03x_%s", id, name);
}
static int msg_data_type(FILE *c, can_msg_t *msg, bool data)
{
	assert(c);
	assert(msg);
	char name[MAX_NAME_LENGTH] = {0};
	make_name(name, MAX_NAME_LENGTH, msg->name, msg->id);
	return fprintf(c, "\t%s_t %s%s;\n", name, name, data ? "_data" : "");
}

static bool signal_are_min_max_valid(signal_t *sig) 
{
	assert(sig);
	return sig->minimum != sig->maximum;
}
static int64_t signed_max(signal_t *sig)
{
	assert(sig);
	if (sig->bit_length == 64)
		return INT64_MAX;
	return ((1uLL << (sig->bit_length - 1)) - 1uLL);
}

static int64_t signed_min(signal_t *sig)
{
	assert(sig);
	if (sig->bit_length == 64)
		return INT64_MIN;
	return ~signed_max(sig);
}
static uint64_t unsigned_max(signal_t *sig) 
{
	assert(sig);
	if (sig->bit_length == 64)
		return UINT64_MAX;
	return (1uLL << (sig->bit_length)) - 1uLL;
}
static int signal2scaling_encode(const char *msgname, unsigned id, signal_t *sig, FILE *o, bool header, dbc2c_options_t *copts) 
{
	assert(msgname);
	assert(sig);
	assert(o);
	assert(copts);
	const char *type = determine_type(sig->bit_length, sig->is_signed);
	if (sig->scaling != 1.0 || sig->offset != 0.0)
		type = "float";
    if (header)fprintf(o, "\t\t");
    fprintf(o, "int ");
    if (!header)
      fprintf(o, "Msg%s::", msgname);
	fprintf(o, "encode_%s(%s in)", sig->name, copts->use_doubles_for_encoding ? "float" : type);
	if (header)
		return fputs(";\n", o);
	fputs(" {\n", o);
	if (signal_are_min_max_valid(sig)) {
		bool gmax = true;
		bool gmin = true;

		if (sig->is_signed) {
			gmin = sig->minimum > signed_min(sig);
			gmax = sig->maximum < signed_max(sig);
		} else {
			gmin = sig->minimum > 0.0;
			gmax = sig->maximum < unsigned_max(sig);
		}
		if (sig->is_floating) {
			gmax = true;
			gmax = true;
		}

		if (gmin || gmax)
			fprintf(o, "\t%s = 0;\n", sig->name); // cast!
		if (gmin)
			fprintf(o, "\tif (in < %g)\n\t\treturn -1;\n", sig->minimum);
		if (gmax)
			fprintf(o, "\tif (in > %g)\n\t\treturn -1;\n", sig->maximum);
	}

	if (sig->scaling == 0.0)
		error("invalid scaling factor (fix your DBC file)");
	if (sig->offset != 0.0)
		fprintf(o, "\tin += %g;\n", -1.0 * sig->offset);
	if (sig->scaling != 1.0)
		fprintf(o, "\tin *= %g;\n", 1.0 / sig->scaling);
	fprintf(o, "\t%s = in;\n", sig->name); // cast!
	return fputs("\treturn 0;\n}\n\n", o);
}

static int signal2scaling_decode(const char *msgname, unsigned id, signal_t *sig, FILE *o, bool header,  dbc2c_options_t *copts) 
{
	assert(msgname);
	assert(sig);
	assert(o);
	assert(copts);
	const char *type = determine_type(sig->bit_length, sig->is_signed);
	if (sig->scaling != 1.0 || sig->offset != 0.0)
		type = "float";
    if (header)fprintf(o, "\t\t");
    fprintf(o, "int ");
    if (!header)
      fprintf(o, "Msg%s::", msgname);
	fprintf(o, "decode_%s(%s *out)", sig->name, copts->use_doubles_for_encoding ? "float" : type);
	if (header)
		return fputs(";\n", o);
	fputs(" {\n", o);
	fprintf(o, "\t%s rval = (%s)(%s);\n", type, type, sig->name);
	if (sig->scaling == 0.0)
		error("invalid scaling factor (fix your DBC file)");
	if (sig->scaling != 1.0)
		fprintf(o, "\trval *= %g;\n", sig->scaling);
	if (sig->offset != 0.0)
		fprintf(o, "\trval += %g;\n", sig->offset);
	if (signal_are_min_max_valid(sig)) {
		bool gmax = true;
		bool gmin = true;

		if (sig->is_signed) { /**@warning comparison may fail because of limits of double size */
			gmin = sig->minimum > signed_min(sig);
			gmax = sig->maximum < signed_max(sig);
		} else {
			gmin = sig->minimum > 0.0;
			gmax = sig->maximum < unsigned_max(sig);
		}
		if (sig->is_floating) {
			gmax = true;
			gmax = true;
		}

		if (!gmax && !gmin) {
			fputs("\t*out = rval;\n", o);
			fputs("\treturn 0;\n", o);
		} else {
			if (gmin && gmax) {
				fprintf(o, "\tif ((rval >= %g) && (rval <= %g)) {\n", sig->minimum, sig->maximum);
			} else if (gmax) {
				fprintf(o, "\tif (rval <= %g) {\n", sig->maximum);
			} else if (gmin) {
				fprintf(o, "\tif (rval >= %g) {\n", sig->minimum);
			}
			fputs("\t\t*out = rval;\n", o);
			fputs("\t\treturn 0;\n", o);
			fputs("\t} else {\n", o);
			fprintf(o, "\t\t*out = (%s)0;\n", type);
			fputs("\t\treturn -1;\n", o);
			fputs("\t}\n", o);

		}
	

	} else {
		fputs("\t*out = rval;\n", o);
		fputs("\treturn 0;\n", o);
	}
	return fputs("}\n\n", o);
}

static unsigned fix_start_bit(bool motorola, unsigned start, unsigned siglen)
{
	if (motorola)
		start = (8 * (7 - (start / 8))) + (start % 8) - (siglen - 1);
	return start;
}
static int comment(signal_t *sig, FILE *o, const char *indent)
{
	assert(sig);
	assert(o);
	return fprintf(o, "%s/* %s: start-bit %u, length %u, endianess %s, scaling %g, offset %g */\n",
			indent,
			sig->name,
			sig->start_bit,
			sig->bit_length,
			sig->endianess == endianess_motorola_e ? "motorola" : "intel",
			sig->scaling,
			sig->offset);
}
static int signal2deserializer(signal_t *sig, const char *msg_name, FILE *o, const char *indent)
{
	assert(sig);
	assert(msg_name);
	assert(o);
	const bool motorola   = (sig->endianess == endianess_motorola_e);
	const unsigned start  = fix_start_bit(motorola, sig->start_bit, sig->bit_length);
	const unsigned length = sig->bit_length;
	const uint64_t mask = length == 64 ?
		0xFFFFFFFFFFFFFFFFuLL :
		(1uLL << length) - 1uLL;

	if (comment(sig, o, indent) < 0)
		return -1;

	if (start)
		fprintf(o, "%sx = (%c >> %d) & 0x%"PRIx64";\n", indent, motorola ? 'm' : 'i', start, mask);
	else
		fprintf(o, "%sx = %c & 0x%"PRIx64";\n", indent, motorola ? 'm' : 'i',  mask);

	if (sig->is_floating) {
		assert(length == 32 || length == 64);
		if (fprintf(o, "%so->%s.%s = unpack754_%d(x);\n", indent, msg_name, sig->name, length) < 0)
			return -1;
		return 0;
	}

	if (sig->is_signed) {
		const uint64_t top = (1uL << (length - 1));
		uint64_t negative = ~mask;
		if (length <= 32)
			negative &= 0xFFFFFFFF;
		if (length <= 16)
			negative &= 0xFFFF;
		if (length <= 8)
			negative &= 0xFF;
		if (negative)
			fprintf(o, "%sx = x & 0x%"PRIx64" ? x | 0x%"PRIx64" : x; \n", indent, top, negative);
	}

	fprintf(o, "%so->%s.%s = x;\n", indent, msg_name, sig->name);
	return 0;
}

static int signal2serializer(signal_t *sig, const char *msg_name, FILE *o, const char *indent)
{
	assert(sig);
	assert(o);
	bool motorola = (sig->endianess == endianess_motorola_e);
	int start = fix_start_bit(motorola, sig->start_bit, sig->bit_length);

	uint64_t mask = sig->bit_length == 64 ?
		0xFFFFFFFFFFFFFFFFuLL :
		(1uLL << sig->bit_length) - 1uLL;

	if (comment(sig, o, indent) < 0)
		return -1;

	if (sig->is_floating) {
		assert(sig->bit_length == 32 || sig->bit_length == 64);
		fprintf(o, "%sx = pack754_%u(o->%s.%s) & 0x%"PRIx64";\n", indent, sig->bit_length, msg_name, sig->name, mask);
	} else {
		fprintf(o, "%sx = ((%s)(o->%s.%s)) & 0x%"PRIx64";\n", indent, determine_unsigned_type(sig->bit_length), msg_name, sig->name, mask);
	}
	if (start)
		fprintf(o, "%sx <<= %u; \n", indent, start);
	fprintf(o, "%s%c |= x;\n", indent, motorola ? 'm' : 'i');
	return 0;
}

static int msg2h(can_msg_t *msg, FILE *h, dbc2c_options_t *copts)
{
	assert(msg);
	assert(h);
	assert(copts);
	char *object_name = duplicate(msg->name);
	const size_t object_name_len = strlen(object_name);
	for (size_t i = 0; i < object_name_len; i++)
		object_name[i] = (isalnum(object_name[i])) ?  tolower(object_name[i]) : '_';
    object_name[0] = (isalnum(object_name[0])) ?  toupper(object_name[0]) : '_';
	fprintf(h, "class Msg%s{\n", object_name);
    fprintf(h, "\tpublic:\n");
    fprintf(h, "\t\tMsg%s();\n", object_name);
    for (size_t i = 0; i < msg->signal_count; i++) {
      fprintf(h, "\t");
      signal2type(msg->sigs[i], h);
      signal2scaling_encode(object_name, msg->id, msg->sigs[i], h, true, copts);
      signal2scaling_decode(object_name, msg->id, msg->sigs[i], h, true, copts);
    }
    fprintf(h, "\n\t\tuint32_t pack(uint64_t *data);");
    fprintf(h, "\n\t\tvoid unpack(MsgCan msg);");
	fprintf(h, "\n};\n");
    fprintf(h, "extern Msg%s msg%s;\n\n", object_name, object_name);
	/* make_name(name, MAX_NAME_LENGTH, msg->name, msg->id); */
    /*  */
	/* for (size_t i = 0; i < msg->signal_count; i++) { */
	/* 	if (copts->generate_unpack) */
	/* 		if (signal2scaling(name, msg->id, msg->sigs[i], h, true, true, god, copts) < 0) */
	/* 			return -1; */
	/* 	if (copts->generate_pack) */
	/* 		if (signal2scaling(name, msg->id, msg->sigs[i], h, false, true, god, copts) < 0) */
	/* 			return -1; */
	/* } */
	/* fputs("\n\n", h); */
	return 0;
}

static int msg2c(can_msg_t *msg, FILE *c, dbc2c_options_t *copts)
{
	assert(msg);
	assert(c);
	assert(copts);
	char *object_name = duplicate(msg->name);
	const size_t object_name_len = strlen(object_name);
	for (size_t i = 0; i < object_name_len; i++)
		object_name[i] = (isalnum(object_name[i])) ?  tolower(object_name[i]) : '_';
    object_name[0] = (isalnum(object_name[0])) ?  toupper(object_name[0]) : '_';
    fprintf(c, "Msg%s msg%s;\n\n", object_name, object_name);
    for (size_t i = 0; i < msg->signal_count; i++) {
      signal2scaling_encode(object_name, msg->id, msg->sigs[i], c, false, copts);
      signal2scaling_decode(object_name, msg->id, msg->sigs[i], c, false, copts);

    }
	/* make_name(name, MAX_NAME_LENGTH, msg->name, msg->id); */
    /*  */
	/* for (size_t i = 0; i < msg->signal_count; i++) { */
	/* 	if (copts->generate_unpack) */
	/* 		if (signal2scaling(name, msg->id, msg->sigs[i], h, true, true, god, copts) < 0) */
	/* 			return -1; */
	/* 	if (copts->generate_pack) */
	/* 		if (signal2scaling(name, msg->id, msg->sigs[i], h, false, true, god, copts) < 0) */
	/* 			return -1; */
	/* } */
	/* fputs("\n\n", h); */
	return 0;
}

int dbc2cpp(dbc_t *dbc, FILE *c, FILE *h, const char *name, dbc2c_options_t *copts)
{
	/**@todo print out ECU node information */
	assert(dbc);
	assert(c);
	assert(h);
	assert(name);
	assert(copts);
	time_t rawtime = time(NULL);
	struct tm *timeinfo = localtime(&rawtime); // This is not considered safe on Visual Studio
	char *file_guard = duplicate(name);
	const size_t file_guard_len = strlen(file_guard);

	/* make file guard all upper case alphanumeric only, first character
	 * alpha only*/
	if (!isalpha(file_guard[0]))
		file_guard[0] = '_';
	for (size_t i = 0; i < file_guard_len; i++)
		file_guard[i] = (isalnum(file_guard[i])) ?  toupper(file_guard[i]) : '_';

	/* sort signals by id */
	qsort(dbc->messages, dbc->message_count, sizeof(dbc->messages[0]), message_compare_function);

	/* sort by size for better struct packing */
	for (size_t i = 0; i < dbc->message_count; i++) {
		can_msg_t *msg = dbc->messages[i];
		qsort(msg->sigs, msg->signal_count, sizeof(msg->sigs[0]), signal_compare_function);
	}

	/* header file (begin) */
	fprintf(h, "/** CAN message encoder/decoder: automatically generated - do not edit\n");
	if (copts->use_time_stamps)
		fprintf(h, "  * @note  Generated on %s", asctime(timeinfo));

	fprintf(h,
		"  * Generated by dbcc: See https://github.com/howerj/dbcc */\n"
		"#ifndef %s\n"
		"#define %s\n\n"
		"#include <stdint.h>\n"
		"%s\n\n",
		file_guard, 
		file_guard,
		copts->generate_print   ? "#include <stdio.h>"  : "");
        fprintf(h, "class MsgCan{\n");
        fprintf(h, "\tpublic:\n");
        fprintf(h, "\t\tuint32_t msgID;\n");
        fprintf(h, "\t\tuint32_t msgDLC;\n");
        fprintf(h, "\t\tuint64_t msgData;\n");
        fprintf(h, "\t\tuint32_t getID(){return msgID;}\n");
        fprintf(h, "\t\tuint32_t getDLC(){return msgDLC;}\n");
        fprintf(h, "\t\tuint64_t getData(){return msgData;}\n");
        fprintf(h, "\t\tuint32_t getDataH(){return (msgData >> 32);}\n");
        fprintf(h, "\t\tuint32_t getDataL(){return (msgData & 0xFFFFFFFF);}\n");
        fprintf(h, "};");


	fputs("\n", h);

	for (size_t i = 0; i < dbc->message_count; i++)
		if (msg2h(dbc->messages[i], h, copts) < 0)
			return -1;

    fputs("#endif\n", h);
        /* header file (end) */

	/* CPP FILE */
	fputs("/* Generated by DBCC, see <https://github.com/howerj/dbcc> */\n", c);
	fprintf(c, "#include \"%s\"\n", name);
	fprintf(c, "#include <inttypes.h>\n");


    for (size_t i=0; i < dbc->message_count; i++)
      if (msg2c(dbc->messages[i], c, copts)<0)
        return -1;

    return 1;
}

