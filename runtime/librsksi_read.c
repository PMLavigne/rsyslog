/* librsksi_read.c - rsyslog's guardtime support library
 * This includes functions used for reading signature (and 
 * other related) files. Well, actually it also contains
 * some writing functionality, but only as far as rsyslog
 * itself is not concerned, but "just" the utility programs.
 *
 * This part of the library uses C stdio and expects that the
 * caller will open and close the file to be read itself.
 *
 * Copyright 2013-2015 Adiscon GmbH.
 *
 * This file is part of rsyslog.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * 
 *       http://www.apache.org/licenses/LICENSE-2.0
 *       -or-
 *       see COPYING.ASL20 in the source distribution
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <assert.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <ksi/ksi.h>

#include "librsgt_common.h"
#include "librsksi.h"

typedef unsigned char uchar;
#ifndef VERSION
#define VERSION "no-version"
#endif
#define MAXFNAME 1024

static int rsksi_read_debug = 0;
char *rsksi_read_puburl = ""; /* old default http://verify.guardtime.com/gt-controlpublications.bin";*/
char *rsksi_extend_puburl = ""; /* old default "http://verifier.guardtime.net/gt-extendingservice";*/
char *rsksi_userid = "";
char *rsksi_userkey = "";
uint8_t rsksi_read_showVerified = 0;

/* macro to obtain next char from file including error tracking */
#define NEXTC	if((c = fgetc(fp)) == EOF) { \
			r = feof(fp) ? RSGTE_EOF : RSGTE_IO; \
			goto done; \
		}

/* if verbose==0, only the first and last two octets are shown,
 * otherwise everything.
 */
static void
outputHexBlob(FILE *fp, const uint8_t *blob, const uint16_t len, const uint8_t verbose)
{
	unsigned i;
	if(verbose || len <= 8) {
		for(i = 0 ; i < len ; ++i)
			fprintf(fp, "%2.2x", blob[i]);
	} else {
		fprintf(fp, "%2.2x%2.2x%2.2x[...]%2.2x%2.2x%2.2x",
			blob[0], blob[1], blob[2],
			blob[len-3], blob[len-2], blob[len-1]);
	}
}

void
outputKSIHash(FILE *fp, char *hdr, const KSI_DataHash *const __restrict__ hash, const uint8_t verbose)
{
	const unsigned char *digest;
	size_t digest_len;
	KSI_DataHash_extract(hash, NULL, &digest, &digest_len); // TODO: error check

	fprintf(fp, "%s", hdr);
	outputHexBlob(fp, digest, digest_len, verbose);
	fputc('\n', fp);
}

void
outputHash(FILE *fp, const char *hdr, const uint8_t *data, const uint16_t len, const uint8_t verbose)
{
	fprintf(fp, "%s", hdr);
	outputHexBlob(fp, data, len, verbose);
	fputc('\n', fp);
}

void
rsksi_errctxInit(ksierrctx_t *ectx)
{
	ectx->fp = NULL;
	ectx->filename = NULL;
	ectx->recNum = 0;
	ectx->ksistate = 0;
	ectx->recNumInFile = 0;
	ectx->blkNum = 0;
	ectx->verbose = 0;
	ectx->errRec = NULL;
	ectx->frstRecInBlk = NULL;
	ectx->fileHash = NULL;
	ectx->lefthash = ectx->righthash = ectx->computedHash = NULL;
}
void
rsksi_errctxExit(ksierrctx_t *ectx)
{
	free(ectx->filename);
	free(ectx->frstRecInBlk);
}

/* note: we do not copy the record, so the caller MUST not destruct
 * it before processing of the record is completed. To remove the
 * current record without setting a new one, call this function
 * with rec==NULL.
 */
void
rsksi_errctxSetErrRec(ksierrctx_t *ectx, char *rec)
{
	ectx->errRec = strdup(rec);
}
/* This stores the block's first record. Here we copy the data,
 * as the caller will usually not preserve it long enough.
 */
void
rsksi_errctxFrstRecInBlk(ksierrctx_t *ectx, char *rec)
{
	free(ectx->frstRecInBlk);
	ectx->frstRecInBlk = strdup(rec);
}

static void
reportError(const int errcode, ksierrctx_t *ectx)
{
	if(ectx->fp != NULL) {
		fprintf(ectx->fp, "%s[%llu:%llu:%llu]: error[%u]: %s\n",
			ectx->filename,
			(long long unsigned) ectx->blkNum, (long long unsigned) ectx->recNum,
			(long long unsigned) ectx->recNumInFile,
			errcode, RSKSIE2String(errcode));
		if(ectx->frstRecInBlk != NULL)
			fprintf(ectx->fp, "\tBlock Start Record.: '%s'\n", ectx->frstRecInBlk);
		if(ectx->errRec != NULL)
			fprintf(ectx->fp, "\tRecord in Question.: '%s'\n", ectx->errRec);
		if(ectx->computedHash != NULL) {
			outputKSIHash(ectx->fp, "\tComputed Hash......: ", ectx->computedHash,
				ectx->verbose);
		}
		if(ectx->fileHash != NULL) {
			outputHash(ectx->fp, "\tSignature File Hash: ", ectx->fileHash->data,
				ectx->fileHash->len, ectx->verbose);
		}
		if(errcode == RSGTE_INVLD_TREE_HASH ||
		   errcode == RSGTE_INVLD_TREE_HASHID) {
			fprintf(ectx->fp, "\tTree Level.........: %d\n", (int) ectx->treeLevel);
			outputKSIHash(ectx->fp, "\tTree Left Hash.....: ", ectx->lefthash,
				ectx->verbose);
			outputKSIHash(ectx->fp, "\tTree Right Hash....: ", ectx->righthash,
				ectx->verbose);
		}
		if(errcode == RSGTE_INVLD_SIGNATURE ||
		   errcode == RSGTE_TS_CREATEHASH) {
			fprintf(ectx->fp, "\tPublication Server.: %s\n", rsksi_read_puburl);
			fprintf(ectx->fp, "\tKSI Verify Signature: [%u]%s\n",
				ectx->ksistate, KSI_getErrorString(ectx->ksistate));
		}
		if(errcode == RSGTE_SIG_EXTEND ||
		   errcode == RSGTE_TS_CREATEHASH) {
			fprintf(ectx->fp, "\tExtending Server...: %s\n", rsksi_extend_puburl);
			fprintf(ectx->fp, "\tKSI Extend Signature: [%u]%s\n",
				ectx->ksistate, KSI_getErrorString(ectx->ksistate));
		}
		if(errcode == RSGTE_TS_DERENCODE) {
			fprintf(ectx->fp, "\tAPI return state...: [%u]%s\n",
				ectx->ksistate, KSI_getErrorString(ectx->ksistate));
		}
	}
}

/* obviously, this is not an error-reporting function. We still use
 * ectx, as it has most information we need.
 */
static void
reportVerifySuccess(ksierrctx_t *ectx) /*OLD CODE , GTVerificationInfo *vrfyInf)*/
{
	fprintf(stdout, "%s[%llu:%llu:%llu]: block signature successfully verified\n",
		ectx->filename,
		(long long unsigned) ectx->blkNum, (long long unsigned) ectx->recNum,
		(long long unsigned) ectx->recNumInFile);
	if(ectx->frstRecInBlk != NULL)
		fprintf(stdout, "\tBlock Start Record.: '%s'\n", ectx->frstRecInBlk);
	if(ectx->errRec != NULL)
		fprintf(stdout, "\tBlock End Record...: '%s'\n", ectx->errRec);
	fprintf(stdout, "\tKSI Verify Signature: [%u]%s\n",
		ectx->ksistate, KSI_getErrorString(ectx->ksistate));
}

/* return the actual length in to-be-written octets of an integer */
static inline uint8_t rsksi_tlvGetInt64OctetSize(uint64_t val)
{
	if(val >> 56)
		return 8;
	if((val >> 48) & 0xff)
		return 7;
	if((val >> 40) & 0xff)
		return 6;
	if((val >> 32) & 0xff)
		return 5;
	if((val >> 24) & 0xff)
		return 4;
	if((val >> 16) & 0xff)
		return 3;
	if((val >> 8) & 0xff)
		return 2;
	return 1;
}

static inline int rsksi_tlvfileAddOctet(FILE *newsigfp, int8_t octet)
{
	/* Directory write into file */
	int r = 0;
	if ( fputc(octet, newsigfp) == EOF ) 
		r = RSGTE_IO; 
done:	return r;
}
static inline int rsksi_tlvfileAddOctetString(FILE *newsigfp, uint8_t *octet, int size)
{
	int i, r = 0;
	for(i = 0 ; i < size ; ++i) {
		r = rsksi_tlvfileAddOctet(newsigfp, octet[i]);
		if(r != 0) goto done;
	}
done:	return r;
}
static inline int rsksi_tlvfileAddInt64(FILE *newsigfp, uint64_t val)
{
	uint8_t doWrite = 0;
	int r;
	if(val >> 56) {
		r = rsksi_tlvfileAddOctet(newsigfp, (val >> 56) & 0xff), doWrite = 1;
		if(r != 0) goto done;
	}
	if(doWrite || ((val >> 48) & 0xff)) {
		r = rsksi_tlvfileAddOctet(newsigfp, (val >> 48) & 0xff), doWrite = 1;
		if(r != 0) goto done;
	}
	if(doWrite || ((val >> 40) & 0xff)) {
		r = rsksi_tlvfileAddOctet(newsigfp, (val >> 40) & 0xff), doWrite = 1;
		if(r != 0) goto done;
	}
	if(doWrite || ((val >> 32) & 0xff)) {
		r = rsksi_tlvfileAddOctet(newsigfp, (val >> 32) & 0xff), doWrite = 1;
		if(r != 0) goto done;
	}
	if(doWrite || ((val >> 24) & 0xff)) {
		r = rsksi_tlvfileAddOctet(newsigfp, (val >> 24) & 0xff), doWrite = 1;
		if(r != 0) goto done;
	}
	if(doWrite || ((val >> 16) & 0xff)) {
		r = rsksi_tlvfileAddOctet(newsigfp, (val >> 16) & 0xff), doWrite = 1;
		if(r != 0) goto done;
	}
	if(doWrite || ((val >> 8) & 0xff)) {
		r = rsksi_tlvfileAddOctet(newsigfp, (val >>  8) & 0xff), doWrite = 1;
		if(r != 0) goto done;
	}
	r = rsksi_tlvfileAddOctet(newsigfp, val & 0xff);
done:	return r;
}

static int
rsksi_tlv8Write(FILE *newsigfp, int flags, int tlvtype, int len)
{
	int r;
	assert((flags & RSGT_TYPE_MASK) == 0);
	assert((tlvtype & RSGT_TYPE_MASK) == tlvtype);
	r = rsksi_tlvfileAddOctet(newsigfp, (flags & ~RSKSI_FLAG_TLV16_RUNTIME) | tlvtype);
	if(r != 0) goto done;
	r = rsksi_tlvfileAddOctet(newsigfp, len & 0xff);
done:	return r;
} 

static int
rsksi_tlv16Write(FILE *newsigfp, int flags, int tlvtype, uint16_t len)
{
	uint16_t typ;
	int r;
	assert((flags & RSGT_TYPE_MASK) == 0);
	assert((tlvtype >> 8 & RSGT_TYPE_MASK) == (tlvtype >> 8));
	typ = ((flags | RSKSI_FLAG_TLV16_RUNTIME) << 8) | tlvtype;
	r = rsksi_tlvfileAddOctet(newsigfp, typ >> 8);
	if(r != 0) goto done;
	r = rsksi_tlvfileAddOctet(newsigfp, typ & 0xff);
	if(r != 0) goto done;
	r = rsksi_tlvfileAddOctet(newsigfp, (len >> 8) & 0xff);
	if(r != 0) goto done;
	r = rsksi_tlvfileAddOctet(newsigfp, len & 0xff);
done:	return r;
} 

/**
 * Write the provided record to the current file position.
 *
 * @param[in] fp file pointer for writing
 * @param[out] rec tlvrecord to write
 *
 * @returns 0 if ok, something else otherwise
 */
int
rsksi_tlvwrite(FILE *fp, tlvrecord_t *rec)
{
	int r = RSGTE_IO;
	if(fwrite(rec->hdr, (size_t) rec->lenHdr, 1, fp) != 1) goto done;
	if(fwrite(rec->data, (size_t) rec->tlvlen, 1, fp) != 1) goto done;
	r = 0;
done:	return r;
}
/*
int
rsksi_tlvWriteHashKSI(FILE *fp, ksifile ksi, uint16_t tlvtype, KSI_DataHash *rec)
{
	unsigned tlvlen;
	int r;
	const unsigned char *digest;
	size_t digest_len;
	r = KSI_DataHash_extract(rec, NULL, &digest, &digest_len); 
	if (r != KSI_OK){
		reportKSIAPIErr(ksi->ctx, ksi, "KSI_DataHash_extract", r);
		goto done;
	}
	tlvlen = 1 + digest_len;
	r = rsksi_tlv16Write(fp, 0x00, tlvtype, tlvlen);
	if(r != 0) goto done;
	r = rsksi_tlvfileAddOctet(fp, hashIdentifierKSI(ksi->hashAlg));
	if(r != 0) goto done;
	r = rsksi_tlvfileAddOctetString(fp, (unsigned char*)digest, digest_len);
done:	return r;
}
*/

/**
 * Read a header from a binary file.
 * @param[in] fp file pointer for processing
 * @param[in] hdr buffer for the header. Must be 9 bytes 
 * 		  (8 for header + NUL byte)
 * @returns 0 if ok, something else otherwise
 */
int
rsksi_tlvrdHeader(FILE *fp, uchar *hdr)
{
	int r;
	if(fread(hdr, 8, 1, fp) != 1) {
		r = RSGTE_IO;
		goto done;
	}
	hdr[8] = '\0';
	r = 0;
done:	return r;
}

/* read type a complete tlv record 
 */
static int
rsksi_tlvRecRead(FILE *fp, tlvrecord_t *rec)
{
	int r = 1;
	int c;

	NEXTC;
	rec->hdr[0] = c;
	rec->tlvtype = c & 0x1f;
	if(c & RSKSI_FLAG_TLV16_RUNTIME) { /* tlv16? */
		rec->lenHdr = 4;
		NEXTC;
		rec->hdr[1] = c;
		rec->tlvtype = (rec->tlvtype << 8) | c;
		NEXTC;
		rec->hdr[2] = c;
		rec->tlvlen = c << 8;
		NEXTC;
		rec->hdr[3] = c;
		rec->tlvlen |= c;
	} else {
		NEXTC;
		rec->lenHdr = 2;
		rec->hdr[1] = c;
		rec->tlvlen = c;
	}
	if(fread(rec->data, (size_t) rec->tlvlen, 1, fp) != 1) {
		r = feof(fp) ? RSGTE_EOF : RSGTE_IO;
		goto done;
	}

	r = 0;
done:	return r;
	if(r == 0 && rsksi_read_debug)
		/* Only show debug if no fail */
		printf("debug: rsksi_tlvRecRead:\t read tlvtype %4.4x, len %u\n", (unsigned) rec->tlvtype,
			(unsigned) rec->tlvlen);
}

/* decode a sub-tlv record from an existing record's memory buffer
 */
static int
rsksi_tlvDecodeSUBREC(tlvrecord_t *rec, uint16_t *stridx, tlvrecord_t *newrec)
{
	int r = 1;
	int c;

	if(rec->tlvlen == *stridx) {r=RSGTE_LEN; if(rsksi_read_debug) printf("debug: rsksi_tlvDecodeSUBREC:\t\t break #1\n"); goto done;}
	c = rec->data[(*stridx)++];
	newrec->hdr[0] = c;
	newrec->tlvtype = c & 0x1f;
	if(c & RSKSI_FLAG_TLV16_RUNTIME) { /* tlv16? */
		newrec->lenHdr = 4;
		if(rec->tlvlen == *stridx) {r=RSGTE_LEN; if(rsksi_read_debug) printf("debug: rsksi_tlvDecodeSUBREC:\t\t break #2\n"); goto done;}
		c = rec->data[(*stridx)++];
		newrec->hdr[1] = c;
		newrec->tlvtype = (newrec->tlvtype << 8) | c;
		if(rec->tlvlen == *stridx) {r=RSGTE_LEN; if(rsksi_read_debug) printf("debug: rsksi_tlvDecodeSUBREC:\t\t break #3\n"); goto done;}
		c = rec->data[(*stridx)++];
		newrec->hdr[2] = c;
		newrec->tlvlen = c << 8;
		if(rec->tlvlen == *stridx) {r=RSGTE_LEN; if(rsksi_read_debug) printf("debug: rsksi_tlvDecodeSUBREC:\t\t break #4\n"); goto done;}
		c = rec->data[(*stridx)++];
		newrec->hdr[3] = c;
		newrec->tlvlen |= c;
	} else {
		if(rec->tlvlen == *stridx) {r=RSGTE_LEN; if(rsksi_read_debug) printf("debug: rsksi_tlvDecodeSUBREC:\t\t break #5\n"); goto done;}
		c = rec->data[(*stridx)++];
		newrec->lenHdr = 2;
		newrec->hdr[1] = c;
		newrec->tlvlen = c;
	}
	if(rec->tlvlen < *stridx + newrec->tlvlen) {r=RSGTE_LEN; if(rsksi_read_debug) printf("debug: rsksi_tlvDecodeSUBREC:\t\t break rec->tlvlen=%d newrec->tlvlen=%d stridx=%d #6\n", rec->tlvlen, newrec->tlvlen, *stridx); goto done;}
	memcpy(newrec->data, (rec->data)+(*stridx), newrec->tlvlen);
	*stridx += newrec->tlvlen;

	if(rsksi_read_debug)
		printf("debug: rsksi_tlvDecodeSUBREC:\t\t Read subtlv: tlvtype %4.4x, len %u\n",
			(unsigned) newrec->tlvtype,
			(unsigned) newrec->tlvlen);
	r = 0;
done:	
	if(r != 0) /* Only on FAIL! */
		printf("debug: rsksi_tlvDecodeSUBREC:\t\t Failed, tlv record %4.4x with error %d\n", rec->tlvtype, r);
	return r;
}

int 
rsksi_tlvDecodeIMPRINT(tlvrecord_t *rec, imprint_t **imprint)
{
	int r = 1;
	imprint_t *imp = NULL;

	if((imp = calloc(1, sizeof(imprint_t))) == NULL) {
		r = RSGTE_OOM;
		goto done;
	}

	imp->hashID = rec->data[0];
	if(rec->tlvlen != 1 + hashOutputLengthOctetsKSI(imp->hashID)) {
		r = RSGTE_LEN;
		goto done;
	}
	imp->len = rec->tlvlen - 1;
	if((imp->data = (uint8_t*)malloc(imp->len)) == NULL) {r=RSGTE_OOM;goto done;}
	memcpy(imp->data, rec->data+1, imp->len);
	*imprint = imp;
	r = 0;
done:	
	if(r == 0) {
		if (rsksi_read_debug) printf("debug: rsksi_tlvDecodeIMPRINT:\t returned %d TLVType=%4.4x, TLVLen=%d, HashID=%d\n", r, rec->tlvtype, rec->tlvlen, imp->hashID);
		if (rsksi_read_debug) outputHash(stdout, "debug: rsksi_tlvDecodeIMPRINT:\t hash: ", imp->data, imp->len, 1);
	} else { 
		/* Free memory on FAIL!*/
		printf("debug: rsksi_tlvDecodeIMPRINT:\t Failed, tlv record %4.4x with error %d\n", rec->tlvtype, r);
		if (imp != NULL)
			rsksi_objfree(rec->tlvtype, imp);
	}
	return r;
}
static int
rsksi_tlvDecodeSIB_HASH(tlvrecord_t *rec, uint16_t *strtidx, imprint_t *imp)
{
	int r = 1;
	tlvrecord_t subrec;

	CHKr(rsksi_tlvDecodeSUBREC(rec, strtidx, &subrec));
	if(!(subrec.tlvtype == 0x02)) { r = RSGTE_INVLTYP; goto done; }
	imp->hashID = subrec.data[0];
	if(subrec.tlvlen != 1 + hashOutputLengthOctetsKSI(imp->hashID)) {
		r = RSGTE_LEN;
		goto done;
	}
	imp->len = subrec.tlvlen - 1;
	if((imp->data = (uint8_t*)malloc(imp->len)) == NULL) {r=RSGTE_OOM;goto done;}
	memcpy(imp->data, subrec.data+1, subrec.tlvlen-1);
	r = 0;
done:	return r;
}
static int
rsksi_tlvDecodeREC_HASH(tlvrecord_t *rec, uint16_t *strtidx, imprint_t *imp)
{
	int r = 1;
	tlvrecord_t subrec;
	CHKr(rsksi_tlvDecodeSUBREC(rec, strtidx, &subrec));
	if(!(subrec.tlvtype == 0x01)) { r = RSGTE_INVLTYP; goto done; }
	imp->hashID = subrec.data[0];

	if(subrec.tlvlen != 1 + hashOutputLengthOctetsKSI(imp->hashID)) {
		if (rsksi_read_debug) printf("debug: rsksi_tlvDecodeREC_HASH:\t\t FAIL on subrec.tlvtype %4.4x subrec.tlvlen = %d\n", subrec.tlvtype, subrec.tlvlen);
		r = RSGTE_LEN;
		goto done;
	}
	imp->len = subrec.tlvlen - 1;
	if((imp->data = (uint8_t*)malloc(imp->len)) == NULL) {r=RSGTE_OOM;goto done;}
	memcpy(imp->data, subrec.data+1, subrec.tlvlen-1);
	r = 0;
done:	
	if(r == 0) {
		if (rsksi_read_debug) printf("debug: rsksi_tlvDecodeREC_HASH:\t\t returned %d TLVType=%4.4x, TLVLen=%d\n", r, rec->tlvtype, rec->tlvlen);
	} else 
		printf("debug: rsksi_tlvDecodeREC_HASH:\t\t Failed, TLVType=%4.4x, TLVLen=%d with error %d\n", rec->tlvtype, rec->tlvlen, r);

	return r;
}
static int
rsksi_tlvDecodeLEVEL_CORR(tlvrecord_t *rec, uint16_t *strtidx, uint8_t *levelcorr)
{
	int r = 1;
	tlvrecord_t subrec;

	CHKr(rsksi_tlvDecodeSUBREC(rec, strtidx, &subrec));
	if(!(subrec.tlvtype == 0x01 && subrec.tlvlen == 1)) {
		if (rsksi_read_debug) printf("debug: rsksi_tlvDecodeLEVEL_CORR:\t FAIL on subrec.tlvtype %4.4x subrec.tlvlen = %d\n", subrec.tlvtype, subrec.tlvlen);
		r = RSGTE_FMT;
		goto done;
	}
	*levelcorr = subrec.data[0];
	r = 0;
done:	
	if(r == 0) {
		if (rsksi_read_debug) printf("debug: rsksi_tlvDecodeLEVEL_CORR:\t returned %d TLVType=%4.4x, TLVLen=%d\n", r, rec->tlvtype, rec->tlvlen);
	} else 
		printf("debug: rsksi_tlvDecodeLEVEL_CORR:\t Failed, tlv record %4.4x with error %d\n", rec->tlvtype, r);
	return r;
}

static int
rsksi_tlvDecodeHASH_STEP(tlvrecord_t *rec, uint16_t *pstrtidx, block_hashstep_t *blhashstep)
{
	int r = 1;
	uint16_t strtidx = 0;
	tlvrecord_t subrec;
/*
	block_hashstep_t *hashstep = NULL;
	if((hashstep = calloc(1, sizeof(block_hashstep_t))) == NULL) {
		r = RSGTE_OOM;
		goto done;
	}
*/
	blhashstep->sib_hash.data = NULL; 

	CHKr(rsksi_tlvDecodeSUBREC(rec, pstrtidx, &subrec)); /* Add to external counter */

	/* Extract HASH-STEP */
	CHKr(rsksi_tlvDecodeLEVEL_CORR(&subrec, &strtidx, &(blhashstep->level_corr)));
	CHKr(rsksi_tlvDecodeSIB_HASH(&subrec, &strtidx, &(blhashstep->sib_hash)));

	if(strtidx != subrec.tlvlen) {
		r = RSGTE_LEN;
		goto done;
	} 

// 	*blhashstep = hashstep;
	r = 0;
done:	
	if (r == 0) {
		if(rsksi_read_debug) printf("debug: rsksi_tlvDecodeHASH_STEP:\t returned %d, tlvtype %4.4x\n", r, (unsigned) rec->tlvtype);
	} else { 
		/* Free memory on FAIL!*/
		printf("debug: rsksi_tlvDecodeHASH_STEP:\t Failed, tlv record %4.4x with error %d\n", rec->tlvtype, r);
		if (blhashstep != NULL) {
			if (blhashstep->sib_hash.data != NULL)
				free(blhashstep->sib_hash.data); 
			free(blhashstep); 
		}
	}
	return r;
}
int 
rsksi_tlvDecodeHASH_CHAIN(tlvrecord_t *rec, block_hashchain_t **blhashchain)
{
	int r = 1;
	uint16_t strtidx = 0;
	block_hashchain_t *hashchain = NULL;
	if((hashchain = calloc(1, sizeof(block_hashchain_t))) == NULL) {
		r = RSGTE_OOM;
		goto done;
	}
	hashchain->rec_hash.data = NULL; 
	hashchain->left_link.sib_hash.data = NULL; 
	hashchain->right_link.sib_hash.data = NULL; 

	/* Extract hash chain */
	CHKr(rsksi_tlvDecodeREC_HASH(rec, &strtidx, &(hashchain->rec_hash)));
	CHKr(rsksi_tlvDecodeHASH_STEP(rec, &strtidx, &(hashchain->left_link)));
	CHKr(rsksi_tlvDecodeHASH_STEP(rec, &strtidx, &(hashchain->right_link)));

	*blhashchain = hashchain;
	r = 0;
done:	
	if(r == 0) {
		if (rsksi_read_debug) printf("debug: rsksi_tlvDecodeHASH_CHAIN:\t returned %d TLVType=%4.4x, TLVLen=%d\n", r, rec->tlvtype, rec->tlvlen);
	} else { 
		/* Free memory on FAIL!*/
		printf("debug: rsksi_tlvDecodeHASH_CHAIN:\t Failed, TLVType=%4.4x, TLVLen=%d with error %d\n", rec->tlvtype, rec->tlvlen, r);
		if (hashchain != NULL)
			rsksi_objfree(rec->tlvtype, hashchain);
	}
	return r;
}

static int
rsksi_tlvDecodeHASH_ALGO(tlvrecord_t *rec, uint16_t *strtidx, uint8_t *hashAlg)
{
	int r = 1;
	tlvrecord_t subrec;

	CHKr(rsksi_tlvDecodeSUBREC(rec, strtidx, &subrec));
	if(!(subrec.tlvtype == 0x01 && subrec.tlvlen == 1)) {
		r = RSGTE_FMT;
		goto done;
	}
	*hashAlg = subrec.data[0];
	r = 0;
done:	return r;
}
static int
rsksi_tlvDecodeBLOCK_IV(tlvrecord_t *rec, uint16_t *strtidx, uint8_t **iv)
{
	int r = 1;
	tlvrecord_t subrec;

	CHKr(rsksi_tlvDecodeSUBREC(rec, strtidx, &subrec));
	if(!(subrec.tlvtype == 0x02)) {
		r = RSGTE_INVLTYP;
		goto done;
	}
	if((*iv = (uint8_t*)malloc(subrec.tlvlen)) == NULL) {r=RSGTE_OOM;goto done;}
	memcpy(*iv, subrec.data, subrec.tlvlen);
	r = 0;
done:	return r;
}
static int
rsksi_tlvDecodeLAST_HASH(tlvrecord_t *rec, uint16_t *strtidx, imprint_t *imp)
{
	int r = 1;
	tlvrecord_t subrec;

	CHKr(rsksi_tlvDecodeSUBREC(rec, strtidx, &subrec));
	if(!(subrec.tlvtype == 0x03)) { r = RSGTE_INVLTYP; goto done; }
	imp->hashID = subrec.data[0];
	if(subrec.tlvlen != 1 + hashOutputLengthOctetsKSI(imp->hashID)) {
		r = RSGTE_LEN;
		goto done;
	}
	imp->len = subrec.tlvlen - 1;
	if((imp->data = (uint8_t*)malloc(imp->len)) == NULL) {r=RSGTE_OOM;goto done;}
	memcpy(imp->data, subrec.data+1, subrec.tlvlen-1);
	r = 0;
done:	return r;
}
static int
rsksi_tlvDecodeREC_COUNT(tlvrecord_t *rec, uint16_t *strtidx, uint64_t *cnt)
{
	int r = 1;
	int i;
	uint64_t val;
	tlvrecord_t subrec;

	CHKr(rsksi_tlvDecodeSUBREC(rec, strtidx, &subrec));
	if(!(subrec.tlvtype == 0x01 && subrec.tlvlen <= 8)) { r = RSGTE_INVLTYP; goto done; }
	val = 0;
	for(i = 0 ; i < subrec.tlvlen ; ++i) {
		val = (val << 8) + subrec.data[i];
	}
	*cnt = val;
	r = 0;
done:	return r;
}
static int
rsksi_tlvDecodeSIG(tlvrecord_t *rec, uint16_t *strtidx, block_sig_t *bs)
{
	int r = 1;
	tlvrecord_t subrec;

	CHKr(rsksi_tlvDecodeSUBREC(rec, strtidx, &subrec));
	if(!(subrec.tlvtype == 0x0905)) { r = RSGTE_INVLTYP; goto done; }
	bs->sig.der.len = subrec.tlvlen;
	bs->sigID = SIGID_RFC3161;
	if((bs->sig.der.data = (uint8_t*)malloc(bs->sig.der.len)) == NULL) {r=RSGTE_OOM;goto done;}
	memcpy(bs->sig.der.data, subrec.data, bs->sig.der.len);
	r = 0;
done:	
	if(rsksi_read_debug) printf("debug: rsksi_tlvDecodeSIG:\t returned %d, tlvtype %4.4x\n", r, (unsigned) rec->tlvtype);
	return r;
}

static int
rsksi_tlvDecodeBLOCK_HDR(tlvrecord_t *rec, block_hdr_t **blockhdr)
{
	int r = 1;
	uint16_t strtidx = 0;
	block_hdr_t *bh = NULL;
	if((bh = calloc(1, sizeof(block_hdr_t))) == NULL) {
		r = RSGTE_OOM;
		goto done;
	}
	CHKr(rsksi_tlvDecodeHASH_ALGO(rec, &strtidx, &(bh->hashID)));
	CHKr(rsksi_tlvDecodeBLOCK_IV(rec, &strtidx, &(bh->iv)));
	CHKr(rsksi_tlvDecodeLAST_HASH(rec, &strtidx, &(bh->lastHash)));
	if(strtidx != rec->tlvlen) {
		r = RSGTE_LEN;
		goto done;
	}
	*blockhdr = bh;
	r = 0;
done:	
	if (r == 0) {
		if(rsksi_read_debug) printf("debug: tlvDecodeBLOCK_HDR:\t returned %d, tlvtype %4.4x\n", r, (unsigned) rec->tlvtype);
	} else { 
		/* Free memory on FAIL!*/
		if (bh != NULL)
			rsksi_objfree(rec->tlvtype, bh);
	}
	return r;
}

static int
rsksi_tlvDecodeEXCERPT_SIG(tlvrecord_t *rec, block_sig_t **blocksig)
{
	int r = 1;
	block_sig_t *bs = NULL;
	if((bs = calloc(1, sizeof(block_sig_t))) == NULL) {
		r = RSGTE_OOM;
		goto done;
	}
	
	/* Read signature now */
	if(!(rec->tlvtype == 0x0905)) { r = RSGTE_INVLTYP; goto done; }
	bs->recCount = 0;
	bs->sig.der.len = rec->tlvlen;
	bs->sigID = SIGID_RFC3161;
	if((bs->sig.der.data = (uint8_t*)malloc(bs->sig.der.len)) == NULL) {r=RSGTE_OOM;goto done;}
	memcpy(bs->sig.der.data, rec->data, bs->sig.der.len);

	*blocksig = bs;
	r = 0;
done:	
	if(r == 0) {
		if (rsksi_read_debug) printf("debug: tlvDecodeEXCERPT_SIG:\t returned %d, tlvtype %4.4x\n", r, (unsigned) rec->tlvtype);
	} else { 
		/* Free memory on FAIL!*/
		if (bs != NULL)
			rsksi_objfree(rec->tlvtype, bs);
	}	
	return r;
}
static int
rsksi_tlvDecodeBLOCK_SIG(tlvrecord_t *rec, block_sig_t **blocksig)
{
	int r = 1;
	uint16_t strtidx = 0;
	block_sig_t *bs = NULL;
	if((bs = calloc(1, sizeof(block_sig_t))) == NULL) {
		r = RSGTE_OOM;
		goto done;
	}
	CHKr(rsksi_tlvDecodeREC_COUNT(rec, &strtidx, &(bs->recCount)));
	CHKr(rsksi_tlvDecodeSIG(rec, &strtidx, bs));
	if(strtidx != rec->tlvlen) {
		r = RSGTE_LEN;
		goto done;
	}
	*blocksig = bs;
	r = 0;
done:	
	if(r == 0) {
		if (rsksi_read_debug) printf("debug: tlvDecodeBLOCK_SIG:\t returned %d, tlvtype %4.4x, recCount %ju\n", r, (unsigned) rec->tlvtype, bs->recCount);
	} else { 
		/* Free memory on FAIL!*/
		if (bs != NULL)
			rsksi_objfree(rec->tlvtype, bs);
	}	
	return r;
}
int
rsksi_tlvRecDecode(tlvrecord_t *rec, void *obj)
{
	int r = 1;
	switch(rec->tlvtype) {
		case 0x0901:
			r = rsksi_tlvDecodeBLOCK_HDR(rec, obj);
			if(r != 0) goto done;
			break;
		case 0x0902:
		case 0x0903:
			r = rsksi_tlvDecodeIMPRINT(rec, obj);
			if(r != 0) goto done;
			break;
		case 0x0904:
			r = rsksi_tlvDecodeBLOCK_SIG(rec, obj);
			if(r != 0) goto done;
			break;
		case 0x0905:
			r = rsksi_tlvDecodeEXCERPT_SIG(rec, obj);
			if(r != 0) goto done;
			break;
		case 0x0907:
			r = rsksi_tlvDecodeHASH_CHAIN(rec, obj);
			if(r != 0) goto done;
			break;
	}
done:
	if(rsksi_read_debug) printf("debug: rsksi_tlvRecDecode:\t\t returned %d, tlvtype %4.4x\n", r, (unsigned) rec->tlvtype);
	return r;
}

static int
rsksi_tlvrdRecHash(FILE *fp, FILE *outfp, imprint_t **imp)
{
	int r;
	tlvrecord_t rec;

	if((r = rsksi_tlvrd(fp, &rec, imp)) != 0) goto done;
	if(rec.tlvtype != 0x0902) {
		if(rsksi_read_debug) printf("debug: rsksi_tlvrdRecHash:\t\t expected tlvtype 0x0902, but was %4.4x\n", rec.tlvtype);
		r = RSGTE_MISS_REC_HASH;
		rsksi_objfree(rec.tlvtype, *imp);
		*imp = NULL; 
		goto done;
	}
	if(outfp != NULL) {
		if((r = rsksi_tlvwrite(outfp, &rec)) != 0) goto done;
	}
	r = 0;
done:	
	if(r == 0 && rsksi_read_debug)
		printf("debug: tlvrdRecHash:\t returned %d, rec->tlvtype %4.4x\n", r, (unsigned) rec.tlvtype);
	return r;
}

static int
rsksi_tlvrdTreeHash(FILE *fp, FILE *outfp, imprint_t **imp)
{
	int r;
	tlvrecord_t rec;

	if((r = rsksi_tlvrd(fp, &rec, imp)) != 0) goto done;
	if(rec.tlvtype != 0x0903) {
		if(rsksi_read_debug) printf("debug: rsksi_tlvrdTreeHash:\t expected tlvtype 0x0903, but was %4.4x\n", rec.tlvtype);
		r = RSGTE_MISS_TREE_HASH;
		rsksi_objfree(rec.tlvtype, *imp);
		*imp = NULL; 
		goto done;
	}
	if(outfp != NULL) {
		if((r = rsksi_tlvwrite(outfp, &rec)) != 0) goto done;
	}
	r = 0;
done:	
	if(r == 0 && rsksi_read_debug) printf("debug: rsksi_tlvrdTreeHash:\t returned %d, rec->tlvtype %4.4x\n", r, (unsigned) rec.tlvtype);
	return r;
}

/* read BLOCK_SIG during verification phase */
static int
rsksi_tlvrdVrfyBlockSig(FILE *fp, block_sig_t **bs, tlvrecord_t *rec)
{
	int r;

	if((r = rsksi_tlvrd(fp, rec, bs)) != 0) goto done;
	if(rec->tlvtype != 0x0904) {
		if(rsksi_read_debug) printf("debug: rsksi_tlvrdVrfyBlockSig:\t expected tlvtype 0x0904, but was %4.4x\n", rec->tlvtype);
		r = RSGTE_MISS_BLOCKSIG;
		/* NOT HERE, done above ! rsksi_objfree(rec->tlvtype, *bs); */
		goto done;
	}
	r = 0;
done:	return r;
}

/**
 * Read the next "object" from file. This usually is
 * a single TLV, but may be something larger, for
 * example in case of a block-sig TLV record.
 * Unknown type records are ignored (or run aborted
 * if we are not permitted to skip).
 *
 * @param[in] fp file pointer for processing
 * @param[out] tlvtype type of tlv record (top-level for
 * 		    structured objects.
 * @param[out] tlvlen length of the tlv record value
 * @param[out] obj pointer to object; This is a proper
 * 		   tlv record structure, which must be casted
 * 		   by the caller according to the reported type.
 * 		   The object must be freed by the caller (TODO: better way?)
 *
 * @returns 0 if ok, something else otherwise
 */
int
rsksi_tlvrd(FILE *fp, tlvrecord_t *rec, void *obj)
{
	int r;
	if((r = rsksi_tlvRecRead(fp, rec)) != 0) goto done;
	r = rsksi_tlvRecDecode(rec, obj);
done:	return r;
}


/* return if a blob is all zero */
static inline int
blobIsZero(uint8_t *blob, uint16_t len)
{
	int i;
	for(i = 0 ; i < len ; ++i)
		if(blob[i] != 0)
			return 0;
	return 1;
}

static void
rsksi_printIMPRINT(FILE *fp, char *name, imprint_t *imp, uint8_t verbose)
{
	fprintf(fp, "%s", name);
		outputHexBlob(fp, imp->data, imp->len, verbose);
		fputc('\n', fp);
}

static void
rsksi_printREC_HASH(FILE *fp, imprint_t *imp, uint8_t verbose)
{
	rsksi_printIMPRINT(fp, "[0x0902]Record hash: ",
		imp, verbose);
}

static void
rsksi_printINT_HASH(FILE *fp, imprint_t *imp, uint8_t verbose)
{
	rsksi_printIMPRINT(fp, "[0x0903]Tree hash..: ",
		imp, verbose);
}

/**
 * Output a human-readable representation of a block_hdr_t
 * to proviced file pointer. This function is mainly inteded for
 * debugging purposes or dumping tlv files.
 *
 * @param[in] fp file pointer to send output to
 * @param[in] bsig ponter to block_hdr_t to output
 * @param[in] verbose if 0, abbreviate blob hexdump, else complete
 */
void
rsksi_printBLOCK_HDR(FILE *fp, block_hdr_t *bh, uint8_t verbose)
 {
	fprintf(fp, "[0x0901]Block Header Record:\n");
 	fprintf(fp, "\tPrevious Block Hash:\n");
	fprintf(fp, "\t   Algorithm..: %s\n", hashAlgNameKSI(bh->lastHash.hashID));
 	fprintf(fp, "\t   Hash.......: ");
		outputHexBlob(fp, bh->lastHash.data, bh->lastHash.len, verbose);
 		fputc('\n', fp);
	if(blobIsZero(bh->lastHash.data, bh->lastHash.len))
 		fprintf(fp, "\t   NOTE: New Hash Chain Start!\n");
	fprintf(fp, "\tHash Algorithm: %s\n", hashAlgNameKSI(bh->hashID));
 	fprintf(fp, "\tIV............: ");
		outputHexBlob(fp, bh->iv, getIVLenKSI(bh), verbose);
 		fputc('\n', fp);
}


/**
 * Output a human-readable representation of a block_sig_t
 * to proviced file pointer. This function is mainly inteded for
 * debugging purposes or dumping tlv files.
 *
 * @param[in] fp file pointer to send output to
 * @param[in] bsig ponter to block_sig_t to output
 * @param[in] verbose if 0, abbreviate blob hexdump, else complete
 */
void
rsksi_printBLOCK_SIG(FILE *fp, block_sig_t *bs, uint8_t verbose)
{
	fprintf(fp, "[0x0904]Block Signature Record:\n");
	fprintf(fp, "\tRecord Count..: %llu\n", (long long unsigned) bs->recCount);
	fprintf(fp, "\tSignature Type: %s\n", sigTypeName(bs->sigID));
	fprintf(fp, "\tSignature Len.: %u\n", (unsigned) bs->sig.der.len);
	fprintf(fp, "\tSignature.....: ");
		outputHexBlob(fp, bs->sig.der.data, bs->sig.der.len, verbose);
		fputc('\n', fp);
}


/**
 * Output a human-readable representation of a tlv object.
 *
 * @param[in] fp file pointer to send output to
 * @param[in] tlvtype type of tlv object (record)
 * @param[in] verbose if 0, abbreviate blob hexdump, else complete
 */
void
rsksi_tlvprint(FILE *fp, uint16_t tlvtype, void *obj, uint8_t verbose)
{
	switch(tlvtype) {
	case 0x0901:
		rsksi_printBLOCK_HDR(fp, obj, verbose);
		break;
	case 0x0902:
		rsksi_printREC_HASH(fp, obj, verbose);
		break;
	case 0x0903:
		rsksi_printINT_HASH(fp, obj, verbose);
		break;
	case 0x0904:
		rsksi_printBLOCK_SIG(fp, obj, verbose);
		break;
	default:fprintf(fp, "rsksi_tlvprint :\t unknown tlv record %4.4x\n", tlvtype);
		break;
	}
}

/**
 * Free the provided object.
 *
 * @param[in] tlvtype type of tlv object (record)
 * @param[in] obj the object to be destructed
 */
void
rsksi_objfree(uint16_t tlvtype, void *obj)
{
	// check if obj is valid 
	if (obj == NULL )
		return; 

	switch(tlvtype) {
	case 0x0901:
		if ( ((block_hdr_t*)obj)->iv != NULL)
			free(((block_hdr_t*)obj)->iv);
		if ( ((block_hdr_t*)obj)->lastHash.data != NULL)
			free(((block_hdr_t*)obj)->lastHash.data);
		break;
	case 0x0902:
	case 0x0903:
		free(((imprint_t*)obj)->data);
		break;
	case 0x0904: /* signature data for a log block */
	case 0x0905: /* signature data for a log block */
		if ( ((block_sig_t*)obj)->sig.der.data != NULL) {
			free(((block_sig_t*)obj)->sig.der.data);
		}
		break;
	case 0x0907: /* Free Hash Chain */
		if ( ((block_hashchain_t*)obj)->rec_hash.data != NULL) {
			free(((block_hashchain_t*)obj)->rec_hash.data);
		}
		if ( ((block_hashchain_t*)obj)->left_link.sib_hash.data != NULL) {
			free(((block_hashchain_t*)obj)->left_link.sib_hash.data);
		}
		if ( ((block_hashchain_t*)obj)->right_link.sib_hash.data != NULL) {
			free(((block_hashchain_t*)obj)->right_link.sib_hash.data);
		}
		break;
	default:fprintf(stderr, "rsksi_objfree:\t unknown tlv record %4.4x\n", tlvtype);
		break;
	}
	free(obj);
}

/**
 * Read block parameters. This detects if the block contains the
 * individual log hashes, the intermediate hashes and the overall
 * block parameters (from the signature block). As we do not have any
 * begin of block record, we do not know e.g. the hash algorithm or IV
 * until reading the block signature record. And because the file is
 * purely sequential and variable size, we need to read all records up to
 * the next signature record.
 * If a caller intends to verify a log file based on the parameters,
 * he must re-read the file from the begining (we could keep things
 * in memory, but this is impractical for large blocks). In order
 * to facitate this, the function permits to rewind to the original
 * read location when it is done.
 *
 * @param[in] fp file pointer of tlv file
 * @param[in] bRewind 0 - do not rewind at end of procesing, 1 - do so
 * @param[out] bs block signature record
 * @param[out] bHasRecHashes 0 if record hashes are present, 1 otherwise
 * @param[out] bHasIntermedHashes 0 if intermediate hashes are present,
 *                1 otherwise
 *
 * @returns 0 if ok, something else otherwise
 */
int
rsksi_getBlockParams(ksifile ksi, FILE *fp, uint8_t bRewind, block_sig_t **bs, 
		block_hdr_t **bh, uint8_t *bHasRecHashes, uint8_t *bHasIntermedHashes)
{
	int r = RSGTE_SUCCESS;
	uint64_t nRecs = 0;
	uint8_t bDone = 0;
	uint8_t bHdr = 0;
	off_t rewindPos = 0;
	void *obj;
	tlvrecord_t rec;

	if(bRewind)
		rewindPos = ftello(fp);
	*bHasRecHashes = 0;
	*bHasIntermedHashes = 0;
	*bs = NULL;
	*bh = NULL;

	while(!bDone) { /* we will err out on EOF */
		if((r = rsksi_tlvrd(fp, &rec, &obj)) != 0) goto done;
		bHdr = 0;
		switch(rec.tlvtype) {
		case 0x0901:
			*bh = (block_hdr_t*) obj;
			bHdr = 1;
			break;
		case 0x0902:
			++nRecs;
			*bHasRecHashes = 1;
			break;
		case 0x0903:
			*bHasIntermedHashes = 1;
/* TODO MAY DELETE LATER */
if (ksi != NULL) {
	/* Free MEM first! */
	if (ksi->x_roothash != NULL) {
		free(ksi->x_roothash->data),
		free(ksi->x_roothash);
	}
	/* Not the best solution but right now we copy each recordhash, the last one will be the root hash */
	if((ksi->x_roothash = calloc(1, sizeof(imprint_t))) == NULL) { r = RSGTE_OOM; goto done; }
	ksi->x_roothash->hashID = ((imprint_t*)obj)->hashID;
	ksi->x_roothash->len = ((imprint_t*)obj)->len;
	if((ksi->x_roothash->data = (uint8_t*)malloc(ksi->x_roothash->len)) == NULL) {r=RSGTE_OOM;goto done;}
	memcpy(ksi->x_roothash->data, ((imprint_t*)obj)->data, ((imprint_t*)obj)->len);
}
			break;
		case 0x0904:
			*bs = (block_sig_t*) obj;
			bDone = 1;
			break;
		default:fprintf(fp, "unknown tlv record %4.4x\n", rec.tlvtype);
			break;
		}
		if(!bDone && !bHdr)
			rsksi_objfree(rec.tlvtype, obj);
	}

	if(*bHasRecHashes && (nRecs != (*bs)->recCount)) {
		r = RSGTE_INVLD_RECCNT;
		goto done;
	}

	if(bRewind) {
		if(fseeko(fp, rewindPos, SEEK_SET) != 0) {
			r = RSGTE_IO;
			goto done;
		}
	}
done:
	if(rsksi_read_debug && r != RSGTE_EOF && r != RSGTE_SUCCESS) printf("debug: rsksi_getBlockParams:\t returned %d\n", r);
	return r;
}

/**
 * Read Excerpt block parameters. This detects if the block contains 
 * hash chains for log records. 
 * If a caller intends to verify a log file based on the parameters,
 * he must re-read the file from the begining (we could keep things
 * in memory, but this is impractical for large blocks). In order
 * to facitate this, the function permits to rewind to the original
 * read location when it is done.
 *
 * @param[in] fp file pointer of tlv file
 * @param[in] bRewind 0 - do not rewind at end of procesing, 1 - do so
 * @param[out] bs block signature record
 *
 * @returns 0 if ok, something else otherwise
 */
int
rsksi_getExcerptBlockParams(ksifile ksi, FILE *fp, uint8_t bRewind, block_sig_t **bs, block_hdr_t **bh)
{
	int r = RSGTE_SUCCESS;
	uint64_t nRecs = 0;
	uint8_t bSig = 0;
	off_t rewindPos = 0;
	void *obj;
	tlvrecord_t rec;

	/* Initial RewindPos */
	if(bRewind) rewindPos = ftello(fp);
	*bs = NULL;

	/* Init Blockheader */
	if((*bh = calloc(1, sizeof(block_hdr_t))) == NULL) {
		r = RSGTE_OOM;
		goto done;
	}
	(*bh)->iv = NULL; 
	(*bh)->lastHash.data = NULL; 

	while(r == RSGTE_SUCCESS && bSig == 0) { /* we will err out on EOF */
		if((r = rsksi_tlvrd(fp, &rec, &obj)) != 0) goto done;
		switch(rec.tlvtype) {
		case 0x0905: /* OpenKSI signature | Excerpt File */
			if (*bs == NULL ) {
				*bs = (block_sig_t*) obj;

				/* Save NEW RewindPos */
				if(bRewind) rewindPos = ftello(fp);
			} else {
				/* Previous Block finished */
				bSig = 1;
			}
			break;
		case 0x0907: /* hash chain for one log record | Excerpt File */
			if (bs != NULL) {
				if (nRecs == 0) /* Copy HASHID from record hash */
					(*bh)->hashID = ((block_hashchain_t*)obj)->rec_hash.hashID; 
				/* Increment hash chain count */
				nRecs++;
			}
			break;
		default:fprintf(fp, "unknown tlv record %4.4x\n", rec.tlvtype);
			break;
		}
		
		/* Free second Signatur object if set! */
		if(bSig == 1 && obj != NULL) rsksi_objfree(rec.tlvtype, obj);
	}
done:
	if (*bs != NULL && r == RSGTE_EOF) {
		if(rsksi_read_debug) printf("debug: rsksi_getExcerptBlockParams:\t Reached END of FILE\n");
		r = RSGTE_SUCCESS;
	} else 
		goto done2; 
	
	/* Copy Count back! */
	(*bs)->recCount = nRecs; 

	/* Rewind file back */
	if(bRewind) {
		if(fseeko(fp, rewindPos, SEEK_SET) != 0) {
			r = RSGTE_IO;
			goto done2;
		}
	}
done2:
	if(rsksi_read_debug) printf("debug: rsksi_getExcerptBlockParams:\t Found %lld records, returned %d\n", (long long unsigned)nRecs, r);
	return r;
}

/**
 * Read the file header and compare it to the expected value.
 * The file pointer is placed right after the header.
 * @param[in] fp file pointer of tlv file
 * @param[in] excpect expected header (e.g. "LOGSIG10")
 * @returns 0 if ok, something else otherwise
 */
int
rsksi_chkFileHdr(FILE *fp, char *expect, uint8_t verbose)
{
	int r;
	char hdr[9];
	off_t rewindPos = ftello(fp);

	if((r = rsksi_tlvrdHeader(fp, (uchar*)hdr)) != 0) goto done;
	if(strcmp(hdr, expect)) {
		r = RSGTE_INVLHDR;
		fseeko(fp, rewindPos, SEEK_SET); /* Reset Filepointer on failure for additional checks*/
	}
	else
		r = 0;
done:
	if(r != RSGTE_SUCCESS && verbose)
		printf("rsksi_chkFileHdr:\t\t failed expected '%s' but was '%s'\n", expect, hdr);
	return r;
}

ksifile
rsksi_vrfyConstruct_gf(void)
{
	int ksistate;
	ksifile ksi;
	if((ksi = calloc(1, sizeof(struct ksifile_s))) == NULL)
		goto done;
	ksi->x_prev = NULL;
	ksi->x_prevleft = NULL;
	ksi->x_prevright = NULL;

	/* Create new KSI Context! */
	rsksictx ctx = rsksiCtxNew();
	ksi->ctx = ctx; /* assign context to ksifile */

	/* Setting KSI Publication URL ! */ 

	ksistate = KSI_CTX_setPublicationUrl(ksi->ctx->ksi_ctx, rsksi_read_puburl);
	if(ksistate != KSI_OK) {
		fprintf(stderr, "Failed setting KSI Publication URL '%s' with error (%d): %s\n", rsksi_read_puburl, ksistate, KSI_getErrorString(ksistate));
		free(ksi);
		return NULL;
	}
	if(rsksi_read_debug)
		fprintf(stdout, "PublicationUrl set to: '%s'\n", rsksi_read_puburl);

	/* Setting KSI Extender! */ 
	ksistate = KSI_CTX_setExtender(ksi->ctx->ksi_ctx, rsksi_extend_puburl, rsksi_userid, rsksi_userkey);
	if(ksistate != KSI_OK) {
		fprintf(stderr, "Failed setting KSIExtender URL '%s' with error (%d): %s\n", rsksi_extend_puburl, ksistate, KSI_getErrorString(ksistate));
		free(ksi);
		return NULL;
	}
	if(rsksi_read_debug)
		fprintf(stdout, "ExtenderUrl set to: '%s'\n", rsksi_extend_puburl);

done:	return ksi;
}

void
rsksi_vrfyBlkInit(ksifile ksi, block_hdr_t *bh, uint8_t bHasRecHashes, uint8_t bHasIntermedHashes)
{
	ksi->hashAlg = hashID2AlgKSI(bh->hashID);
	ksi->bKeepRecordHashes = bHasRecHashes;
	ksi->bKeepTreeHashes = bHasIntermedHashes;
	if (ksi->IV != NULL ) {
		free(ksi->IV);
		ksi->IV = NULL; 
	}
	if (bh->iv != NULL)	{
		ksi->IV = malloc(getIVLenKSI(bh));
		memcpy(ksi->IV, bh->iv, getIVLenKSI(bh));
	}
	if (bh->lastHash.data != NULL ) {
		ksi->x_prev = malloc(sizeof(imprint_t));
		ksi->x_prev->len=bh->lastHash.len;
		ksi->x_prev->hashID = bh->lastHash.hashID;
		ksi->x_prev->data = malloc(ksi->x_prev->len);
		memcpy(ksi->x_prev->data, bh->lastHash.data, ksi->x_prev->len);
	} else {
		ksi->x_prev = NULL; 
	}
}

static int
rsksi_vrfy_chkRecHash(ksifile ksi, FILE *sigfp, FILE *nsigfp, 
		     KSI_DataHash *hash, ksierrctx_t *ectx)
{
	int r = 0;
	imprint_t *imp = NULL;

	const unsigned char *digest;
	KSI_DataHash_extract(hash, NULL, &digest, NULL); // TODO: error check

	if((r = rsksi_tlvrdRecHash(sigfp, nsigfp, &imp)) != 0)
		reportError(r, ectx);
		goto done;
	if(imp->hashID != hashIdentifierKSI(ksi->hashAlg)) {
		reportError(r, ectx);
		r = RSGTE_INVLD_REC_HASHID;
		goto done;
	}
	if(memcmp(imp->data, digest,
		  hashOutputLengthOctetsKSI(imp->hashID))) {
		r = RSGTE_INVLD_REC_HASH;
		ectx->computedHash = hash;
		ectx->fileHash = imp;
		reportError(r, ectx);
		ectx->computedHash = NULL, ectx->fileHash = NULL;
		goto done;
	}
	r = 0;
done:
	if(imp != NULL)
		rsksi_objfree(0x0902, imp);
	return r;
}

static int
rsksi_vrfy_chkTreeHash(ksifile ksi, FILE *sigfp, FILE *nsigfp,
                      KSI_DataHash *hash, ksierrctx_t *ectx)
{
	int r = 0;
	imprint_t *imp = NULL;
	const unsigned char *digest;
	KSI_DataHash_extract(hash, NULL, &digest, NULL); // TODO: error check


	if((r = rsksi_tlvrdTreeHash(sigfp, nsigfp, &imp)) != 0) {
		reportError(r, ectx);
		goto done;
	}
	if(imp->hashID != hashIdentifierKSI(ksi->hashAlg)) {
		reportError(r, ectx);
		r = RSGTE_INVLD_TREE_HASHID;
		goto done;
	}
	if(memcmp(imp->data, digest, hashOutputLengthOctetsKSI(imp->hashID))) {
		r = RSGTE_INVLD_TREE_HASH;
		ectx->computedHash = hash;
		ectx->fileHash = imp;
		reportError(r, ectx);
		ectx->computedHash = NULL, ectx->fileHash = NULL;
		goto done;
	} else {
		/* EXTRA DEBUG !!!! */
		if(rsksi_read_debug) {
			ectx->computedHash = hash;
			ectx->fileHash = imp;
			printf("debug: rsksi_vrfy_chkTreeHash:\t DEBUG OUTPUT\n");
			if(ectx->frstRecInBlk != NULL) fprintf(stdout, "\tBlock Start Record.: '%s'\n", ectx->frstRecInBlk);
			if(ectx->errRec != NULL) fprintf(stdout, "\tRecord in Question.: '%s'\n", ectx->errRec);
			if(ectx->computedHash != NULL) outputKSIHash(stdout, "\tComputed Hash......: ", ectx->computedHash, ectx->verbose);
			if(ectx->fileHash != NULL) outputHash(stdout, "\tSignature File Hash: ", ectx->fileHash->data, ectx->fileHash->len, ectx->verbose); 
			outputKSIHash(stdout, "\tTree Left Hash.....: ", ectx->lefthash, ectx->verbose);
			outputKSIHash(stdout, "\tTree Right Hash....: ", ectx->righthash, ectx->verbose);
			ectx->computedHash = NULL, ectx->fileHash = NULL;
		}
	}
	r = 0;
done:
	if(imp != NULL) {
		if(rsksi_read_debug) printf("debug: rsksi_vrfy_chkTreeHash:\t returned %d, hashID=%d, Length=%d\n", r, imp->hashID, hashOutputLengthOctetsKSI(imp->hashID));
		/* Free memory */
		rsksi_objfree(0x0903, imp);
	}
	return r;
}

/* Helper function to verifiy the next record in the signature file */
int
rsksi_vrfy_nextRec(ksifile ksi, FILE *sigfp, FILE *nsigfp, unsigned char *rec, size_t len, ksierrctx_t *ectx, int storehashchain)
{
	int r = 0;
	KSI_DataHash *x; /* current hash */
	KSI_DataHash *m, *recHash = NULL, *t, *t_del;
	uint8_t j;

	hash_m_ksi(ksi, &m);
	hash_r_ksi(ksi, &recHash, rec, len);

	if(ksi->bKeepRecordHashes) {
		r = rsksi_vrfy_chkRecHash(ksi, sigfp, nsigfp, recHash, ectx);
		if(r != 0) goto done;
	}
	hash_node_ksi(ksi, &x, m, recHash, 1); /* hash leaf */
	if(ksi->bKeepTreeHashes) {
		ectx->treeLevel = 0;
		ectx->lefthash = m;
		ectx->righthash = recHash;
		r = rsksi_vrfy_chkTreeHash(ksi, sigfp, nsigfp, x, ectx);
		if(r != 0) goto done;
	}
/* EXTRA DEBUG !!!! */
if(rsksi_read_debug) {
	outputKSIHash(stdout, "\tTree Left Hash.....: ", m, ectx->verbose);
	outputKSIHash(stdout, "\tTree Right Hash....: ", recHash, ectx->verbose);
	outputKSIHash(stdout, "\tTree Current Hash....: ", x, ectx->verbose);
}

	if (storehashchain == 1) {
		/* Store Left Hash for extraction */
		rsksiimprintDel(ksi->x_prevleft);
		ksi->x_prevleft = rsksiImprintFromKSI_DataHash(ksi, m);

		//	rsksiimprintDel(ksi->x_prevright);
		//	ksi->x_prevright = rsksiImprintFromKSI_DataHash(ksi, recHash);
	}

	/* Store Current Hash for later use */
	rsksiimprintDel(ksi->x_prev);
	ksi->x_prev = rsksiImprintFromKSI_DataHash(ksi, x);

	/* add x to the forest as new leaf, update roots list */
	t = x;
if(rsksi_read_debug) printf("debug: rsksi_vrfy_nextRec:\t nRoots = (%d)\n", ksi->nRoots);
	for(j = 0 ; j < ksi->nRoots ; ++j) {
		if(ksi->roots_valid[j] == 0) {
			ksi->roots_hash[j] = t;
			ksi->roots_valid[j] = 1;
			t = NULL;
			break;
		} else if(t != NULL) {
if(rsksi_read_debug) printf("debug: rsksi_vrfy_nextRec:\t hash interim node level (%d)\n", (j+1));
outputKSIHash(stdout, "\tKSI Root hash!!!!....: ", ksi->roots_hash[j], ectx->verbose);

if (storehashchain == 1) {
	/* Store Right Hash for extraction */
	rsksiimprintDel(ksi->x_prevright);
	ksi->x_prevright = rsksiImprintFromKSI_DataHash(ksi, ksi->roots_hash[j]);
}

			/* hash interim node */
			ectx->treeLevel = j+1;
			ectx->righthash = t;
			t_del = t;
			hash_node_ksi(ksi, &t, ksi->roots_hash[j], t_del, j+2);
			ksi->roots_valid[j] = 0;
			if(ksi->bKeepTreeHashes) {
				ectx->lefthash = ksi->roots_hash[j];
				r = rsksi_vrfy_chkTreeHash(ksi, sigfp, nsigfp, t, ectx);
				if(r != 0) goto done; /* mem leak ok, we terminate! */
			}
			KSI_DataHash_free(ksi->roots_hash[j]);
			KSI_DataHash_free(t_del);
		}
	}
	if(t != NULL) {

//if (ksi->x_prevright == NULL)
//	ksi->x_prevright = rsksiImprintFromKSI_DataHash(ksi, t);
if(rsksi_read_debug) outputKSIHash(stdout, "\tTree Root Hash....: ", t, ectx->verbose);

		/* new level, append "at the top" */
		ksi->roots_hash[ksi->nRoots] = t;
		ksi->roots_valid[ksi->nRoots] = 1;
		++ksi->nRoots;
		assert(ksi->nRoots < MAX_ROOTS);
		t = NULL;
	}
	++ksi->nRecords;

	/* cleanup */
	KSI_DataHash_free(m);
done:
	if(recHash != NULL) KSI_DataHash_free(recHash);
if(rsksi_read_debug) printf("debug: rsksi_vrfy_nextRec:\t returned %d\n", r);
	return r;
}

/* Helper function to verifiy the next hash chain record in the signature file */
int 
rsksi_vrfy_nextHashChain(block_sig_t *bs, ksifile ksi, FILE *sigfp, unsigned char *rec, size_t len, ksierrctx_t *ectx)
{
	int r = 0;
	int ksistate;
	KSI_Signature *sig = NULL;
	KSI_DataHash *line_hash = NULL, *root_hash = NULL, *root_tmp = NULL; 
	KSI_DataHash *rec_hash = NULL, *left_hash = NULL, *right_hash = NULL; 
	uint8_t j;
	void *obj;
	tlvrecord_t tlvrec;
	block_hashchain_t *blhashchain = NULL; 
	
	/* Check for next valid tlvrecord */
	if ((r = rsksi_tlvrd(sigfp, &tlvrec, &obj)) != 0) goto done;
	if (tlvrec.tlvtype != 0x0907) {
		r = RSGTE_INVLTYP; 
		goto done;
	}
	
	/* Convert Pointer to block_hashchain_t*/ 
	blhashchain = (block_hashchain_t*)obj; 
	
	/* Verify Hash Alg */
	if(blhashchain->rec_hash.hashID != hashIdentifierKSI(ksi->hashAlg)) {
		reportError(r, ectx);
		r = RSGTE_INVLD_REC_HASHID;
		goto done;
	}

	/* Convert imprints into KSI_DataHashes */
	KSI_DataHash_fromDigest (ksi->ctx->ksi_ctx, blhashchain->rec_hash.hashID, blhashchain->rec_hash.data, blhashchain->rec_hash.len, &rec_hash);  	
	KSI_DataHash_fromDigest (ksi->ctx->ksi_ctx, blhashchain->left_link.sib_hash.hashID, blhashchain->left_link.sib_hash.data, blhashchain->left_link.sib_hash.len, &left_hash);  	
	KSI_DataHash_fromDigest (ksi->ctx->ksi_ctx, blhashchain->right_link.sib_hash.hashID, blhashchain->right_link.sib_hash.data, blhashchain->right_link.sib_hash.len, &right_hash);  	
	
	/* Create Root Hash from LINE and Left Sibling */
	hash_r_ksi(ksi, &line_hash, rec, len);
	hash_node_ksi(ksi, &root_hash, left_hash, line_hash, blhashchain->left_link.level_corr + 1); 

	/* Compare root_hash vs rec_hash */
	if ( KSI_DataHash_equals (root_hash, rec_hash) != 1 ) {
		r = RSGTE_INVLD_REC_HASH;
		ectx->computedHash = root_hash;
		ectx->fileHash = &(blhashchain->rec_hash);
		reportError(r, ectx);
		ectx->computedHash = NULL, ectx->fileHash = NULL;
		goto done;
	} else {
		if(rsksi_read_debug) printf("debug: rsksi_vrfy_nextHashChain:\t Success root_hash equals rec_hash\n"); 
	}

//	outputKSIHash(stdout, "\tTree Computed Root Hash.1.: ", root_hash, ectx->verbose);
	/* Create Root hash using Right Sibling */
	root_tmp = root_hash; 
	hash_node_ksi(ksi, &root_hash, right_hash, root_tmp, blhashchain->right_link.level_corr);
	KSI_DataHash_free(root_tmp);
//	outputKSIHash(stdout, "\tTree Computed Root Hash.2.: ", root_hash, ectx->verbose);

/* EXTRA DEBUG !!!! */
if(rsksi_read_debug) {
outputKSIHash(stdout, "\tTree Left Hash............: ", left_hash, ectx->verbose);
outputKSIHash(stdout, "\tTree Right Hash...........: ", right_hash, ectx->verbose);
outputKSIHash(stdout, "\tTree Record Hash..........: ", rec_hash, ectx->verbose);
outputKSIHash(stdout, "\tTree Line Hash.:..........: ", line_hash, ectx->verbose);
outputKSIHash(stdout, "\tTree Computed Root Hash...: ", root_hash, ectx->verbose);
}

	/* Parse KSI Signature */
	ksistate = KSI_Signature_parse(ksi->ctx->ksi_ctx, bs->sig.der.data, bs->sig.der.len, &sig);
	if(ksistate != KSI_OK) {
		if(rsksi_read_debug) printf("debug: rsksi_vrfy_nextHashChain:\t KSI_Signature_parse failed with error: %s (%d)\n", KSI_getErrorString(ksistate), ksistate); 
		r = RSGTE_INVLD_SIGNATURE;
		ectx->ksistate = ksistate;
		goto done;
	} else {
		if(rsksi_read_debug) printf("debug: rsksi_vrfy_nextHashChain:\t KSI_Signature_parse was successfull\n"); 
	}

	/* Verify KSI Signature */
	ksistate = KSI_Signature_verify(sig, ksi->ctx->ksi_ctx);
	if(ksistate != KSI_OK) {
		if(rsksi_read_debug) printf("debug: rsksi_vrfy_nextHashChain:\t KSI_Signature_verify failed with error: %s (%d)\n", KSI_getErrorString(ksistate), ksistate); 
		r = RSGTE_INVLD_SIGNATURE;
		ectx->ksistate = ksistate;
		goto done;
	} else {
		if(rsksi_read_debug) printf("debug: rsksi_vrfy_nextHashChain:\t KSI_Signature_verify was successfull\n"); 
	}
	
	/* Verify Roothash against Signature */
	ksistate = KSI_Signature_verifyDataHash(sig, ksi->ctx->ksi_ctx, root_hash);
	if (ksistate != KSI_OK) {
		if(rsksi_read_debug) printf("debug: rsksi_vrfy_nextHashChain:\t KSI_Signature_verifyDataHash failed with error: %s (%d)\n", KSI_getErrorString(ksistate), ksistate); 
		r = RSGTE_INVLD_SIGNATURE;
		ectx->ksistate = ksistate;
		goto done;
	} else {
		if(rsksi_read_debug) printf("debug: rsksi_vrfy_nextHashChain:\t KSI_Signature_parse was successfull\n"); 
	}
done:
	/* Free Memory */
	if(root_hash != NULL) KSI_DataHash_free(root_hash);
	if(line_hash != NULL) KSI_DataHash_free(line_hash);
	if(rec_hash != NULL) KSI_DataHash_free(rec_hash);
	if(left_hash != NULL) KSI_DataHash_free(left_hash);
	if(right_hash != NULL) KSI_DataHash_free(right_hash);
	if(blhashchain != NULL) rsksi_objfree(0x0907, blhashchain); 

	if(rsksi_read_debug) printf("debug: rsksi_vrfy_nextHashChain:\t returned %d\n", r);
	return r;
}

/* TODO: think about merging this with the writer. The
 * same applies to the other computation algos.
 */
static int
verifySigblkFinish(ksifile ksi, KSI_DataHash **pRoot)
{
	KSI_DataHash *root, *rootDel;
	int8_t j;
	int r;
	root = NULL;

	if(ksi->nRecords == 0) {
		if(rsksi_read_debug) printf("debug: verifySigblkFinish:\t no records!!!%d\n", r);
		goto done;
	}

	for(j = 0 ; j < ksi->nRoots ; ++j) {
		if(root == NULL) {
			root = ksi->roots_valid[j] ? ksi->roots_hash[j] : NULL;
			ksi->roots_valid[j] = 0; /* guess this is redundant with init, maybe del */
		} else if(ksi->roots_valid[j]) {
			rootDel = root;
			hash_node_ksi(ksi, &root, ksi->roots_hash[j], root, j+2);
			ksi->roots_valid[j] = 0; /* guess this is redundant with init, maybe del */
			KSI_DataHash_free(rootDel);
		}
	}

	*pRoot = root;
	r = 0;
done:
	ksi->bInBlk = 0;
	if (rsksi_read_debug && root != NULL) outputKSIHash(stdout, "debug: verifySigblkFinish: Root hash: \t", root, 1);
return r;
}

/* helper for rsksi_extendSig: */
#define COPY_SUBREC_TO_NEWREC \
	memcpy(newrec.data+iWr, subrec.hdr, subrec.lenHdr); \
	iWr += subrec.lenHdr; \
	memcpy(newrec.data+iWr, subrec.data, subrec.tlvlen); \
	iWr += subrec.tlvlen;

static inline int
rsksi_extendSig(KSI_Signature *sig, ksifile ksi, tlvrecord_t *rec, ksierrctx_t *ectx)
{
	KSI_Signature *extended = NULL;
	uint8_t *der = NULL;
	size_t lenDer;
	int r, rgt;
	tlvrecord_t newrec, subrec;
	uint16_t iRd, iWr;

	/* Extend Signature now using KSI API*/
	rgt = KSI_extendSignature(ksi->ctx->ksi_ctx, sig, &extended);
	if (rgt != KSI_OK) {
		ectx->ksistate = rgt;
		r = RSGTE_SIG_EXTEND;
		goto done;
	}

	/* Serialize Signature. */
	rgt = KSI_Signature_serialize(extended, &der, &lenDer);
	if(rgt != KSI_OK) {
		ectx->ksistate = rgt;
		r = RSGTE_SIG_EXTEND;
		goto done;
	}

	/* update block_sig tlv record with new extended timestamp */
	/* we now need to copy all tlv records before the actual der
	 * encoded part.
	 */
	iRd = iWr = 0;
	// TODO; check tlvtypes at comment places below!
	CHKr(rsksi_tlvDecodeSUBREC(rec, &iRd, &subrec)); 
	/* HASH_ALGO */
	COPY_SUBREC_TO_NEWREC
	CHKr(rsksi_tlvDecodeSUBREC(rec, &iRd, &subrec));
	/* BLOCK_IV */
	COPY_SUBREC_TO_NEWREC
	CHKr(rsksi_tlvDecodeSUBREC(rec, &iRd, &subrec));
	/* LAST_HASH */
	COPY_SUBREC_TO_NEWREC
	CHKr(rsksi_tlvDecodeSUBREC(rec, &iRd, &subrec));
	/* REC_COUNT */
	COPY_SUBREC_TO_NEWREC
	CHKr(rsksi_tlvDecodeSUBREC(rec, &iRd, &subrec));
	/* actual sig! */
	newrec.data[iWr++] = 0x09 | RSKSI_FLAG_TLV16_RUNTIME;
	newrec.data[iWr++] = 0x06;
	newrec.data[iWr++] = (lenDer >> 8) & 0xff;
	newrec.data[iWr++] = lenDer & 0xff;
	/* now we know how large the new main record is */
	newrec.tlvlen = (uint16_t) iWr+lenDer;
	newrec.tlvtype = rec->tlvtype;
	newrec.hdr[0] = rec->hdr[0];
	newrec.hdr[1] = rec->hdr[1];
	newrec.hdr[2] = (newrec.tlvlen >> 8) & 0xff;
	newrec.hdr[3] = newrec.tlvlen & 0xff;
	newrec.lenHdr = 4;
	memcpy(newrec.data+iWr, der, lenDer);
	/* and finally copy back new record to existing one */
	memcpy(rec, &newrec, sizeof(newrec)-sizeof(newrec.data)+newrec.tlvlen+4);
	r = 0;
done:
	if(extended != NULL)
		KSI_Signature_free(extended);
	if (der != NULL)
		KSI_free(der);
	return r;
}



/* Verify the existance of the header. 
 */
int
verifyBLOCK_HDRKSI(ksifile ksi, FILE *sigfp, FILE *nsigfp, tlvrecord_t* tlvrec)
{
	int r;
	block_hdr_t *bh = NULL;
	if ((r = rsksi_tlvrd(sigfp, tlvrec, &bh)) != 0) goto done;
	if (tlvrec->tlvtype != 0x0901) {
		if(rsksi_read_debug) printf("debug: verifyBLOCK_HDRKSI:\t expected tlvtype 0x0901, but was %4.4x\n", tlvrec->tlvtype);
		r = RSGTE_MISS_BLOCKSIG;
		goto done;
	}
	if (nsigfp != NULL)
		if ((r = rsksi_tlvwrite(nsigfp, tlvrec)) != 0) goto done; 
done:	
	if (bh != NULL)
		rsksi_objfree(tlvrec->tlvtype, bh);
	if(rsksi_read_debug) printf("debug: verifyBLOCK_HDRKSI:\t returned %d\n", r);
	return r;
}

/* verify the root hash. This also means we need to compute the
 * Merkle tree root for the current block.
 */
int
verifyBLOCK_SIGKSI(block_sig_t *bs, ksifile ksi, FILE *sigfp, FILE *nsigfp,
                uint8_t bExtend, ksierrctx_t *ectx)
{
	int r;
	int ksistate;
	block_sig_t *file_bs = NULL;
	KSI_Signature *sig = NULL;
	KSI_DataHash *ksiHash = NULL;
	tlvrecord_t rec;

	if((r = verifySigblkFinish(ksi, &ksiHash)) != 0)
		goto done;
	if((r = rsksi_tlvrdVrfyBlockSig(sigfp, &file_bs, &rec)) != 0)
		goto done;
	if(ectx->recNum != bs->recCount) {
		r = RSGTE_INVLD_RECCNT;
		goto done;
	}

	/* Parse KSI Signature */
	ksistate = KSI_Signature_parse(ksi->ctx->ksi_ctx, file_bs->sig.der.data, file_bs->sig.der.len, &sig);
	if(ksistate != KSI_OK) {
		if(rsksi_read_debug) printf("debug: verifyBLOCK_SIGKSI:\t KSI_Signature_parse failed with error: %s (%d)\n", KSI_getErrorString(ksistate), ksistate); 
		r = RSGTE_INVLD_SIGNATURE;
		ectx->ksistate = ksistate;
		goto done;
	}
	ksistate = KSI_Signature_verifyDataHash(sig, ksi->ctx->ksi_ctx, ksiHash);
	if (ksistate != KSI_OK) {
		if(rsksi_read_debug) printf("debug: verifyBLOCK_SIGKSI:\t KSI_Signature_verifyDataHash failed with error: %s (%d)\n", KSI_getErrorString(ksistate), ksistate); 
		r = RSGTE_INVLD_SIGNATURE;
		ectx->ksistate = ksistate;
		goto done;
		/* TODO proberly additional verify with KSI_Signature_verify*/
	}

	if(rsksi_read_debug) printf("debug: verifyBLOCK_SIGKSI:\t processed without error's\n"); 
	if(rsksi_read_showVerified)
		reportVerifySuccess(ectx);
	if(bExtend)
		if((r = rsksi_extendSig(sig, ksi, &rec, ectx)) != 0) goto done;
		
	if(nsigfp != NULL) {
if(rsksi_read_debug) printf("debug: verifyBLOCK_SIGKSI:\t WRITE ROOT HASH!!!\n"); 
		if((r = rsksi_tlvwrite(nsigfp, &rec)) != 0) goto done;
	}
	r = 0;
done:
	if(file_bs != NULL)
		rsksi_objfree(0x0904, file_bs);
	if(r != 0)
		reportError(r, ectx);
	if(ksiHash != NULL) 
		KSI_DataHash_free(ksiHash);
	if(sig != NULL)
		KSI_Signature_free(sig);
	return r;
}

/* Helper function to enable debug */
void rsksi_set_debug(int iDebug)
{
	rsksi_read_debug = iDebug; 
}

/* Helper function to convert an old V10 signature file into V11 */
int rsksi_ConvertSigFile(char* name, FILE *oldsigfp, FILE *newsigfp, int verbose)
{
	int r = 0, rRead = 0;
	imprint_t *imp = NULL;
	tlvrecord_t rec;
	tlvrecord_t subrec;
	
	/* For signature convert*/
	int i;
	uint16_t strtidx = 0; 
	block_hdr_t *bh = NULL;
	block_sig_t *bs = NULL;
	uint16_t typconv;
	unsigned tlvlen;
	uint8_t tlvlenRecords;

	/* Temporary change flags back to old default */
	RSKSI_FLAG_TLV16_RUNTIME = 0x20; 

	/* Start reading Sigblocks from old FILE */
	while(1) { /* we will err out on EOF */
		rRead = rsksi_tlvRecRead(oldsigfp, &rec); 
		if(rRead == 0 /*|| rRead == RSGTE_EOF*/) {
			switch(rec.tlvtype) {
				case 0x0900:
				case 0x0901:
					/* Convert tlvrecord Header */
					if (rec.tlvtype == 0x0900) {
						typconv = ((0x00 /*flags*/ | 0x80 /* NEW RSKSI_FLAG_TLV16_RUNTIME*/) << 8) | 0x0902;
						rec.hdr[0] = typconv >> 8; 
						rec.hdr[1] = typconv & 0xff; 
					} else if (rec.tlvtype == 0x0901) {
						typconv = ((0x00 /*flags*/ | 0x80 /* NEW RSKSI_FLAG_TLV16_RUNTIME*/) << 8) | 0x0903;
						rec.hdr[0] = typconv >> 8; 
						rec.hdr[1] = typconv & 0xff; 
					}

					/* Debug verification output */
					r = rsksi_tlvDecodeIMPRINT(&rec, &imp);
					if(r != 0) goto donedecode;
					rsksi_printREC_HASH(stdout, imp, verbose);

					/* Output into new FILE */
					if((r = rsksi_tlvwrite(newsigfp, &rec)) != 0) goto done;

					/* Free mem*/
					free(imp->data);
					free(imp);
					imp = NULL; 
					break;
				case 0x0902:
					/* Split Data into HEADER and BLOCK */
					strtidx = 0;

					/* Create BH and BS*/
					if((bh = calloc(1, sizeof(block_hdr_t))) == NULL) {
						r = RSGTE_OOM;
						goto donedecode;
					}
					if((bs = calloc(1, sizeof(block_sig_t))) == NULL) {
						r = RSGTE_OOM;
						goto donedecode;
					}

					/* Check OLD encoded HASH ALGO */
					CHKrDecode(rsksi_tlvDecodeSUBREC(&rec, &strtidx, &subrec));
					if(!(subrec.tlvtype == 0x00 && subrec.tlvlen == 1)) {
						r = RSGTE_FMT;
						goto donedecode;
					}
					bh->hashID = subrec.data[0];

					/* Check OLD encoded BLOCK_IV */
					CHKrDecode(rsksi_tlvDecodeSUBREC(&rec, &strtidx, &subrec));
					if(!(subrec.tlvtype == 0x01)) {
						r = RSGTE_INVLTYP;
						goto donedecode;
					}
					if((bh->iv = (uint8_t*)malloc(subrec.tlvlen)) == NULL) {r=RSGTE_OOM;goto donedecode;}
					memcpy(bh->iv, subrec.data, subrec.tlvlen);

					/* Check OLD encoded LAST HASH */
					CHKrDecode(rsksi_tlvDecodeSUBREC(&rec, &strtidx, &subrec));
					if(!(subrec.tlvtype == 0x02)) { r = RSGTE_INVLTYP; goto donedecode; }
					bh->lastHash.hashID = subrec.data[0];
					if(subrec.tlvlen != 1 + hashOutputLengthOctetsKSI(bh->lastHash.hashID)) {
						r = RSGTE_LEN;
						goto donedecode;
					}
					bh->lastHash.len = subrec.tlvlen - 1;
					if((bh->lastHash.data = (uint8_t*)malloc(bh->lastHash.len)) == NULL) {r=RSGTE_OOM;goto donedecode;}
					memcpy(bh->lastHash.data, subrec.data+1, subrec.tlvlen-1);

					/* Debug verification output */
					rsksi_printBLOCK_HDR(stdout, bh, verbose);

					/* Check OLD encoded COUNT */
					CHKrDecode(rsksi_tlvDecodeSUBREC(&rec, &strtidx, &subrec));
					if(!(subrec.tlvtype == 0x03 && subrec.tlvlen <= 8)) { r = RSGTE_INVLTYP; goto donedecode; }
					bs->recCount = 0;
					for(i = 0 ; i < subrec.tlvlen ; ++i) {
						bs->recCount = (bs->recCount << 8) + subrec.data[i];
					}

					/* Check OLD encoded SIG */
					CHKrDecode(rsksi_tlvDecodeSUBREC(&rec, &strtidx, &subrec));
					if(!(subrec.tlvtype == 0x0905)) { r = RSGTE_INVLTYP; goto donedecode; }
					bs->sig.der.len = subrec.tlvlen;
					bs->sigID = SIGID_RFC3161;
					if((bs->sig.der.data = (uint8_t*)malloc(bs->sig.der.len)) == NULL) {r=RSGTE_OOM;goto donedecode;}
					memcpy(bs->sig.der.data, subrec.data, bs->sig.der.len);

					/* Debug output */
					rsksi_printBLOCK_SIG(stdout, bs, verbose);

					if(strtidx != rec.tlvlen) {
						r = RSGTE_LEN;
						goto donedecode;
					}

					/* Set back to NEW default */
					RSKSI_FLAG_TLV16_RUNTIME = 0x80; 

					/* Create Block Header */
					tlvlen  = 2 + 1 /* hash algo TLV */ +
						  2 + hashOutputLengthOctetsKSI(bh->hashID) /* iv */ +
						  2 + 1 + bh->lastHash.len /* last hash */;
					/* write top-level TLV object block-hdr */
					CHKrDecode(rsksi_tlv16Write(newsigfp, 0x00, 0x0901, tlvlen));
					/* and now write the children */
					/* hash-algo */
					CHKrDecode(rsksi_tlv8Write(newsigfp, 0x00, 0x01, 1));
					CHKrDecode(rsksi_tlvfileAddOctet(newsigfp, hashIdentifierKSI(bh->hashID)));
					/* block-iv */
					CHKrDecode(rsksi_tlv8Write(newsigfp, 0x00, 0x02, hashOutputLengthOctetsKSI(bh->hashID)));
					CHKrDecode(rsksi_tlvfileAddOctetString(newsigfp, bh->iv, hashOutputLengthOctetsKSI(bh->hashID)));
					/* last-hash */
					CHKrDecode(rsksi_tlv8Write(newsigfp, 0x00, 0x03, bh->lastHash.len + 1));
					CHKrDecode(rsksi_tlvfileAddOctet(newsigfp, bh->lastHash.hashID));
					CHKrDecode(rsksi_tlvfileAddOctetString(newsigfp, bh->lastHash.data, bh->lastHash.len));

					/* Create Block Signature */
					tlvlenRecords = rsksi_tlvGetInt64OctetSize(bs->recCount);
					tlvlen  = 2 + tlvlenRecords /* rec-count */ +
						  4 + bs->sig.der.len /* open-ksi */;
					/* write top-level TLV object (block-sig */
					CHKrDecode(rsksi_tlv16Write(newsigfp, 0x00, 0x0904, tlvlen));
					/* and now write the children */
					/* rec-count */
					CHKrDecode(rsksi_tlv8Write(newsigfp, 0x00, 0x01, tlvlenRecords));
					CHKrDecode(rsksi_tlvfileAddInt64(newsigfp, bs->recCount));
					/* open-ksi */
					CHKrDecode(rsksi_tlv16Write(newsigfp, 0x00, 0x905, bs->sig.der.len));
					CHKrDecode(rsksi_tlvfileAddOctetString(newsigfp, bs->sig.der.data, bs->sig.der.len));

donedecode:
					/* Set back to OLD default */
					RSKSI_FLAG_TLV16_RUNTIME = 0x20; 

					/* Free mem*/
					if (bh != NULL) {
						free(bh->iv);
						free(bh->lastHash.data);
						free(bh);
						bh = NULL; 
					}
					if (bs != NULL) {
						free(bs->sig.der.data);
						free(bs);
						bs = NULL; 
					}
					if(r != 0) goto done;
					break;
				default:
					printf("debug: rsksi_ConvertSigFile:\t unknown tlv record %4.4x\n", rec.tlvtype);
					break;
			}
		} else {
			/*if(feof(oldsigfp))
				break;
			else*/
			r = rRead; 
			if(r == RSGTE_EOF) 
				r = 0; /* Successfully finished file */
			else if(rsksi_read_debug)
				printf("debug: rsksi_ConvertSigFile:\t failed to read with error %d\n", r);
			goto done;
		}

		/* Abort further processing if EOF */
		if (rRead == RSGTE_EOF)
			goto done;
	}
done:
	if(rsksi_read_debug)
		printf("debug: rsksi_ConvertSigFile:\t  returned %d\n", r);
	return r;
}

/* Helper function to write Block header */
int rsksi_StartHashChain(FILE *newsigfp, ksifile ksi, block_sig_t *bsIn, int iRightLinkRecords, int verbose)
{
	int r = 0;
	unsigned tlvlen;
	uint8_t tlvlenLevelCorr;

	if(rsksi_read_debug) printf("debug: rsksi_StartHashChain:\t NEW HashChain started with %d RightLink records\n", iRightLinkRecords);

	/* Error Check */
	if (ksi->x_prevleft == NULL || ksi->x_prev == NULL) {
		r = RSGTE_EXTRACT_HASH; 
		goto done; 
	}

	/* Total Length of Hash Chain */
	tlvlenLevelCorr = rsksi_tlvGetInt64OctetSize(ksi->nRoots);
	tlvlen =	4 + /* ???? */	
				2 + 1 + ksi->x_prev->len												/* rec-hash */ +
				2 + tlvlenLevelCorr + 2 + 1 + ksi->x_prevleft->len						/* left-link */ + 
				((2 + tlvlenLevelCorr + 2 + 1 + ksi->x_prevright->len)*iRightLinkRecords);	/* Count of all other right-links */

	/* Start hash chain for one log record */
	CHKrDecode(rsksi_tlv16Write(newsigfp, 0x00, 0x0907, tlvlen));

	/* rec-hash */
	CHKrDecode(rsksi_tlv8Write(newsigfp, 0x00, 0x01, 1 + ksi->x_prev->len));
	CHKrDecode(rsksi_tlvfileAddOctet(newsigfp, ksi->x_prev->hashID));
	CHKrDecode(rsksi_tlvfileAddOctetString(newsigfp, ksi->x_prev->data, ksi->x_prev->len));
outputHash(stdout, "debug: rsksi_StartHashChain:\t Record Hash: \t\t", ksi->x_prev->data, ksi->x_prev->len, 1);

	/* left-link - step in the hash chain*/
	CHKrDecode(rsksi_tlv8Write(newsigfp, 0x00, 0x02, 2 + tlvlenLevelCorr + 2 + 1 + ksi->x_prevleft->len));
	/* level correction value */
	CHKrDecode(rsksi_tlv8Write(newsigfp, 0x00, 0x01, tlvlenLevelCorr));
	CHKrDecode(rsksi_tlvfileAddInt64(newsigfp, 0)); /* Left HASH is stored with Level 0 ! */
	/* sibling hash value */
	CHKrDecode(rsksi_tlv8Write(newsigfp, 0x00, 0x02, 1 + ksi->x_prevleft->len));
	CHKrDecode(rsksi_tlvfileAddOctet(newsigfp, ksi->x_prevleft->hashID));
	CHKrDecode(rsksi_tlvfileAddOctetString(newsigfp, ksi->x_prevleft->data, ksi->x_prevleft->len));
outputHash(stdout, "debug: rsksi_StartHashChain:\t Left Hash: \t\t", ksi->x_prevleft->data, ksi->x_prevleft->len, 1);

donedecode:
	if(r != 0) printf("debug: rsksi_StartHashChain:\t failed to write with error %d\n", r);
done:
	if(rsksi_read_debug) printf("debug: rsksi_StartHashChain:\t returned %d\n", r);
	return r;
}

/* Helper function to write Block header */
int rsksi_AddRightToHashChain(FILE *newsigfp, ksifile ksi, block_sig_t *bsIn, uint64_t uiLevelCorrectionValue, int verbose)
{
	int r = 0;
	uint8_t tlvlenLevelCorr;
	
	/* Error Check */
	if (ksi->x_prevright == NULL) {
		r = RSGTE_EXTRACT_HASH; 
		goto done; 
	}

	/* Total Length of Hash Chain */
	tlvlenLevelCorr = rsksi_tlvGetInt64OctetSize(uiLevelCorrectionValue);

	/* right-link - step in the hash chain*/
	CHKrDecode(rsksi_tlv8Write(newsigfp, 0x00, 0x03, 2 + tlvlenLevelCorr + 2 + 1 + ksi->x_prevright->len));
	/* level correction value */
	CHKrDecode(rsksi_tlv8Write(newsigfp, 0x00, 0x01, tlvlenLevelCorr));
	CHKrDecode(rsksi_tlvfileAddInt64(newsigfp, uiLevelCorrectionValue)); /* Left HASH is stored with Level 2 ! */
	/* sibling hash value */
	CHKrDecode(rsksi_tlv8Write(newsigfp, 0x00, 0x02, 1 + ksi->x_prevright->len));
	CHKrDecode(rsksi_tlvfileAddOctet(newsigfp, ksi->x_prevright->hashID));
	CHKrDecode(rsksi_tlvfileAddOctetString(newsigfp, ksi->x_prevright->data, ksi->x_prevright->len));
outputHash(stdout, "debug: AddRightToHashChain:\t Right  Hash: \t\t", ksi->x_prevright->data, ksi->x_prevright->len, 1);

donedecode:
	if(r != 0) printf("debug: AddRightToHashChain:\t failed to write with error %d\n", r);
done:
	if(rsksi_read_debug) printf("debug: AddRightToHashChain:\t returned %d\n", r);
	return r;
}

/* Helper function to Extract Block Signature */
int rsksi_ExtractBlockSignature(FILE *newsigfp, ksifile ksi, block_sig_t *bsIn, ksierrctx_t *ectx, int verbose)
{
	int r = 0;

	/* WRITE BLOCK Signature */
	/* open-ksi */
	CHKrDecode(rsksi_tlv16Write(newsigfp, 0x00, 0x905, bsIn->sig.der.len));
	CHKrDecode(rsksi_tlvfileAddOctetString(newsigfp, bsIn->sig.der.data, bsIn->sig.der.len));

donedecode:
	if(r != 0) printf("debug: rsksi_ExtractBlockSignature:\t failed to write with error %d\n", r);
done:
	if(rsksi_read_debug) printf("debug: ExtractBlockSignature:\t returned %d\n", r);
	return r;
}
