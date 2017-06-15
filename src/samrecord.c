#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include <assert.h>
#include "main.h"
#include "align.h"
#include "util.h"
#include "samrecord.h"

uint32_t record_hash(SAMRecord *record)
{
	if (record->hashed)
		return record->hash;

	const uint32_t hash = hash_ident(record->ident)*(record->mate + 1);
	record->hash = hash;
	record->hashed = 1;
	return hash;
}

uint32_t record_hash_mate(SAMRecord *record)
{
	if (record->mate_hashed)
		return record->mate_hash;

	const uint32_t hash = hash_ident(record->ident)*(2 - record->mate);
	record->mate_hash = hash;
	record->mate_hashed = 1;
	return hash;
}

uint32_t record_eq(SAMRecord *r1, SAMRecord *r2)
{
	return record_hash(r1) == record_hash(r2) &&
	       r1->mate == r2->mate &&
	       strcmp(r1->ident, r2->ident) == 0;
}

uint32_t record_eq_mate(SAMRecord *r1, SAMRecord *r2)
{
	return record_hash_mate(r1) == record_hash(r2) &&
	       r1->mate != r2->mate &&
	       strcmp(r1->ident, r2->ident) == 0;
}

int record_cmp(const void *v1, const void *v2) {
	const SAMRecord *r1 = (SAMRecord *)v1;
	const SAMRecord *r2 = (SAMRecord *)v2;

	const bc_t bc1 = r1->bc;
	const bc_t bc2 = r2->bc;
	int c = (bc1 > bc2) - (bc1 < bc2);

	if (c != 0) return c;

	const uint8_t chrom1 = r1->chrom;
	const uint8_t chrom2 = r2->chrom;
	c = (chrom1 > chrom2) - (chrom1 < chrom2);

	if (c != 0) return c;

	const uint32_t pos1 = r1->pos;
	const uint32_t pos2 = r2->pos;
	c = (pos1 > pos2) - (pos1 < pos2);

	if (c != 0) return c;

	return strcmp(r1->ident, r2->ident);
}

int is_dup(SAMRecord *r1, SAMRecord *r2)
{
	return strcmp(r1->ident, r2->ident) == 0 &&
	       r1->chrom == r2->chrom &&
	       r1->pos == r2->pos &&
	       r1->mate == r2->mate &&
	       r1->rev == r2->rev &&
	       r1->score == r2->score;
}

/* caution: frees given records buffer */
SAMRecord *remove_dups(SAMRecord *records, size_t *n_records_ptr)
{
	const size_t n_records = *n_records_ptr;
	SAMRecord *records_no_dups = safe_malloc((n_records+1) * sizeof(*records_no_dups));
	size_t n_records_no_dups = 0;
	size_t i = 0;

	while (i < n_records) {
		SAMRecord *rec = &records[i++];
		records_no_dups[n_records_no_dups++] = *rec;

		while (i < n_records && is_dup(rec, &records[i])) {
			i++;
		}
	}

	free(records);
	*n_records_ptr = n_records_no_dups;
	records_no_dups[n_records_no_dups].bc = 0;
	return records_no_dups;
}

// from BWA source
static inline int get_rlen(int n_cigar, const uint32_t *cigar)
{
	int k, l;
	for (k = l = 0; k < n_cigar; k++) {
		int op = cigar[k] & 0xf;
		if (op == 0 || op == 2)
			l += (cigar[k] >> 4);
	}
	return l;
}

static inline char rc(const char c)
{
	switch (c) {
	case 'A':
		return 'T';
	case 'C':
		return 'G';
	case 'G':
		return 'C';
	case 'T':
		return 'A';
	case 'N':
		return 'N';
	}

	assert(0);
}

void print_sam_record(SAMRecord *rec, SAMRecord *mate, double gamma, FILE *out, const char *rg_id)
{
	assert((rec != NULL || mate != NULL) && !isnan(gamma));
	int flag = SAM_READ_PAIRED;
	char *ident;
	char *chrom = "*";
	uint32_t pos = 0;
	int mapq = 255;
	int read_len;
	bc_t bc;
	SingleReadAlignment *r = NULL;
	FASTQRecord *fq;

	if (rec != NULL) {
		ident = rec->ident;
		chrom = chrom_lookup(rec->chrom);
		pos = rec->pos;
		read_len = (rec->mate == 0 ? MATE1_LEN : MATE2_LEN);
		bc = rec->bc;
		r = &rec->aln;
		fq = rec->fq;

		const double gamma_phred = -10.0*log(1.0 - gamma);
		mapq = (gamma_phred > 253.0) ? 254 : (int)round(gamma_phred);

		if (rec->rev)
			flag |= SAM_READ_REVERSED;

		flag |= ((rec->mate == 0) ? SAM_1ST_IN_PAIR : SAM_2ND_IN_PAIR);
	} else {
		ident = mate->ident;
		read_len = (mate->mate == 0 ? MATE2_LEN : MATE1_LEN);
		bc = mate->bc;
		fq = mate->fq_mate;
		flag |= SAM_READ_UNMAPPED;
		flag |= ((mate->mate == 0) ? SAM_2ND_IN_PAIR : SAM_1ST_IN_PAIR);
	}

	if (mate != NULL) {
		flag |= SAM_READ_PROPER;

		if (mate->rev)
			flag |= SAM_MATE_REVERSED;
	} else {
		flag |= SAM_MATE_UNMAPPED;
	}

	// basics
	fprintf(out, "%s\t%d\t%s\t%u\t%d\t", ident, flag, chrom, pos, mapq);

	// cigar
	if (rec != NULL) {
		const uint32_t *cigar = r->cigar;
		const int cigar_len = r->n_cigar;
		for (int i = 0; i < cigar_len; i++) {
			const uint32_t op   = cigar[i];
			const uint32_t type = op & 0xf;
			const uint32_t n    = op >> 4;
			fprintf(out, "%u%c", n, "MIDSS"[type]);  // note: convert hard to soft clipping
		}
	} else {
		fputc('*', out);
	}

	// mate mapping
	if (mate != NULL) {
		const int same_chrom = (rec != NULL) && (mate->chrom == rec->chrom);
		fprintf(out, "\t%s\t%d", same_chrom ? "=" : chrom_lookup(mate->chrom), mate->pos);

		SingleReadAlignment *s = &mate->aln;
		if (same_chrom) {
			int64_t p0 = r->pos + (r->rev ? get_rlen(r->n_cigar, r->cigar) - 1 : 0);
			int64_t p1 = s->pos + (s->rev ? get_rlen(s->n_cigar, s->cigar) - 1 : 0);
			if (s->n_cigar == 0 || r->n_cigar == 0)
				fprintf(out, "\t0");
			else
				fprintf(out, "\t%lld", (-(p0 - p1 + (p0 > p1 ? 1 : p0 < p1 ? -1 : 0))));
		} else {
			fprintf(out, "\t0");
		}
	} else {
		fprintf(out, "\t*\t0\t0");
	}

	// seq and qual
	fputc('\t', out);
	if (rec != NULL && rec->rev) {
		for (int i = read_len - 1; i >= 0; i--) {
			fputc(rc(fq->read[i]), out);
		}

		fputc('\t', out);

		for (int i = read_len - 1; i >= 0; i--) {
			fputc(fq->qual[i], out);
		}
	} else {
		for (int i = 0; i < read_len; i++) {
			fputc(fq->read[i], out);
		}

		fputc('\t', out);

		for (int i = 0; i < read_len; i++) {
			fputc(fq->qual[i], out);
		}
	}

	// tags
	char bc_str[BC_LEN + 1] = {0};
	decode_bc(bc, bc_str);
	if (rec != NULL) {
		fprintf(out, "\tNM:i:%d\tBX:Z:%s-1\tXG:f:%.5g", r->edit_dist, bc_str, gamma);
	} else {
		fprintf(out, "\tBX:Z:%s-1", bc_str);
	}

	if (rg_id != NULL) {
		fprintf(out, "\tRG:Z:");
		for (size_t i = 0; rg_id[i] != '\0' && !isspace(rg_id[i]); i++)
			fputc(rg_id[i], out);
	}
	fputc('\n', out);
}

