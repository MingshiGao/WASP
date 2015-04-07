
#include <zlib.h>
#include <string.h>
#include <math.h>

#include "util.h"
#include "vcf.h"
#include "memutil.h"

#define VCF_GTYPE_MISSING -1


const char *vcf_fix_headers[] =
  {"#CHROM", "POS", "ID", "REF", "ALT", "QUAL",
   "FILTER", "INFO", "FORMAT"};



void vcf_read_header(gzFile vcf_fh, VCFInfo *vcf_info) {
  char *line, *cur, *token;
  int tok_num;
  int n_fix_header;
  const char delim[] = " \t";

  n_fix_header = sizeof(vcf_fix_headers) / sizeof(const char *);

  vcf_info->n_header_lines = 0;
  
  while(1) {
    line = util_gzgets_line(vcf_fh);

    if(line == NULL) {
      my_err("%s:%d: could not read header information from file",
	     __FILE__, __LINE__);

    }

    if(util_str_starts_with(line, "##")) {
      /* header line */
      vcf_info->n_header_lines += 1;
    }
    else if(util_str_starts_with(line, "#CHROM")) {
      /* this should be last header line that contains list of fixed fields */
      vcf_info->n_header_lines += 1;
	
      cur = line;
      tok_num = 0;
      while((token = strsep(&cur, delim)) != NULL) {
	if(tok_num < n_fix_header) {
	  if(strcmp(token, vcf_fix_headers[tok_num]) != 0) {
	    my_warn("expected token %d to be %s but got '%s'",
		    tok_num, vcf_fix_headers[tok_num], token);
	  }
	}
	tok_num += 1;
      }
      vcf_info->n_samples = tok_num - n_fix_header;
      break;
    } else {
      my_err("expected last line in header to start with #CHROM");
    }
    my_free(line);
  }
}



/**
 * Parses ':'-delimited format string and 
 * returns index of token that matches. 
 * Returns -1 on failure.
 * 
 */
int get_format_index(const char *format_str, const char *label) {
  char fmt[VCF_MAX_FORMAT];
  char delim[] = ":";
  char *cur, *tok;
  int idx, i;

  /* copy format string before tokenizing (which modifies it) */
  strcpy(fmt, format_str);

  /* find index of label format specifier */
  idx = -1;
  i = 0;
  cur = fmt;
  while((tok = strsep(&cur, delim)) != NULL) {
    if(strcmp(tok, label) == 0) {
      /* found label in format string */
      idx = i;
      break;
    }
    i++;
  }

  return idx;
}


void parse_haplotypes(VCFInfo *vcf_info, char *haplotypes,
		      char *cur) {
  int gt_idx, hap1, hap2, i, n;
  static int warn_phase = TRUE;
  long expect_haps, n_haps;
  char gt_str[VCF_MAX_FORMAT];
  char delim[] = " \t";
  char inner_delim[] = ":";
  char *inner_cur, *tok, *inner_tok;

  /* get index of GT token in format string*/
  gt_idx = get_format_index(vcf_info->format, "GT");
  if(gt_idx == -1) {
    my_err("%s:%d: VCF format string does not specify GT token, "
	   "cannot obtain haplotypes: '%s'", __FILE__, __LINE__,
	   vcf_info->format);
  }
  
  expect_haps = vcf_info->n_samples * 2;
  
  n_haps = 0;
  
  while((tok = strsep(&cur, delim)) != NULL) {
    /* Each genotype string is delimited by ':'
     * The GT portions of the string are delimited by '/' or '|'
     * '|' indicates phased, '/' indicates unphased.
     */
    util_strncpy(gt_str, tok, sizeof(gt_str));
    
    i = 0;
    inner_cur = gt_str;
    while((i <= gt_idx) && (inner_tok = strsep(&inner_cur, inner_delim)) != NULL) {
      if(i == gt_idx) {
	n = sscanf(inner_tok, "%d|%d", &hap1, &hap2);
	if(n != 2) {
	  /* try with '/' separator instead */
	  n = sscanf(inner_tok, "%d/%d", &hap1, &hap2);

	  if(n == 2 && warn_phase) {
	    my_warn("%s:%d: some genotypes are unphased (delimited "
		    "with '/' instead of '|')\n", __FILE__, __LINE__,
		    inner_tok);
	    warn_phase = FALSE;
	  } else {
	    my_warn("%s:%d: could not parse genotype string '%s'\n",
		    __FILE__, __LINE__, inner_tok);
	    hap1 = VCF_GTYPE_MISSING;
	    hap2 = VCF_GTYPE_MISSING;
	  }
	}

	if((hap1 != VCF_GTYPE_MISSING && hap1 != 0 && hap1 != 1)  ||
	   (hap2 != VCF_GTYPE_MISSING && hap2 != 0 && hap2 != 1)) {

	  /* Copy number polymorphisms and multi-allelic SNPs
	   * can have values other than 0 and 1 (e.g. 3, 4, ...).
	   * Combined haplotype test does not currently deal with 
	   * these. Set the genotypes to MISSING (-1)
	   */
	  hap1 = VCF_GTYPE_MISSING;
	  hap2 = VCF_GTYPE_MISSING;
	}

	if((n_haps + 2) > expect_haps) {
	  my_err("%s:%d: more genotypes per line than expected",
		 __FILE__, __LINE__);
	}
	haplotypes[n_haps] = hap1;
	haplotypes[n_haps+1] = hap2;

	n_haps += 2;
      }
      
      i++;
    }
  }

  if(n_haps != expect_haps) {
    my_err("%s:%d: expected %ld genotype values per line, but got "
	   "%ld", __FILE__, __LINE__, expect_haps, n_haps);
  }
}



void parse_geno_probs(VCFInfo *vcf_info, float *geno_probs,
		      char *cur) {
  char delim[] = " \t";
  char inner_delim[] = ":";
  char *tok, *inner_tok, *inner_cur;
  char gtype[VCF_MAX_FORMAT];
  long gl_idx, i, n, n_geno_probs, expect_geno_probs;
  float like_homo_ref, like_het, like_homo_alt;
  float prob_homo_ref, prob_het, prob_homo_alt, prob_sum;

  expect_geno_probs = vcf_info->n_samples * 3;
  
  /* get index of GL token in format string*/
  gl_idx = get_format_index(vcf_info->format, "GL");
  if(gl_idx == -1) {
    my_err("%s:%d: VCF format string does not specify GL token, cannot "
	   "obtain genotype probabilities", __FILE__, __LINE__);
  }

  n_geno_probs = 0;
  
  while((tok = strsep(&cur, delim)) != NULL) {
    /* each genotype string is delimited by ':'
     * each GL portion is delimited by ','
     */
    util_strncpy(gtype, tok, sizeof(gtype));

    i = 0;
    inner_cur = gtype;
    while((i <= gl_idx) && (inner_tok = strsep(&inner_cur, inner_delim)) != NULL) {
      if(i == gl_idx) {
	n = sscanf(inner_tok, "%g,%g,%g", &like_homo_ref, &like_het,
		   &like_homo_alt);

	if(n != 3) {
	  if(strcmp(inner_tok, ".") == 0) {
	    /* '.' indicates missing data
	     * set all likelihoods to log(0.333) = -0.477
	     */
	    like_homo_ref = like_het = like_homo_alt = -0.477;
	  } else {
	    my_err("%s:%d: failed to parse genotype likelihoods from "
		   "string '%s'", __FILE__, __LINE__, inner_tok);
	  }
	}

	/* convert log10(prob) to prob */
	prob_homo_ref = pow(10.0, like_homo_ref);
	prob_het = pow(10.0, like_het);
	prob_homo_alt = pow(10.0, like_homo_alt);

	if((n_geno_probs + 3) > expect_geno_probs) {
	  my_err("%s:%d: more genotype likelihoods per line than expected",
		 __FILE__, __LINE__);
	}
	
	/* most of time probs sum to 1.0, but sometimes they do not
	 * possibly reflects different likelihoods used for indel 
	 * calling but not sure. Normalize probs so they sum to 1.0
	 * This is like getting posterior assuming uniform prior.
	 */
	prob_sum = prob_homo_ref + prob_het + prob_homo_alt;
	prob_homo_ref = prob_homo_ref / prob_sum;
	prob_het = prob_het / prob_sum;
	prob_homo_alt = prob_homo_alt / prob_sum;
     	
	geno_probs[n_geno_probs] = prob_homo_ref;
	geno_probs[n_geno_probs + 1] = prob_het;
	geno_probs[n_geno_probs + 2] = prob_homo_alt;

	n_geno_probs += 3;
      }

      i++;
    }
  }

  if(n_geno_probs != expect_geno_probs) {
    my_err("%s:%d: expected %ld genotype likelihoods per line, but got "
	   "%ld", __FILE__, __LINE__, expect_geno_probs, n_geno_probs);
  }
  
}



/**
 * Gets next line of VCF file and parses it into VCFInfo datastructure.
 *
 * If geno_probs array is non-null genotype likelihoods are parsed and
 * stored in the provided array. The array must be of length
 * n_samples*3.
 *
 * If haplotypes array is non-null phased genotypes are parsed and
 * stored in the provided array. The array must be of length
 * n_samples*2.
 *
 * Returns 0 on success, -1 if at EOF.
 */
int vcf_read_line(gzFile vcf_fh, VCFInfo *vcf_info,
		  float *geno_probs, char *haplotypes) {
  char *line, *cur, *token;
  int n_fix_header, ref_len, alt_len;
  size_t tok_num;
  const char delim[] = " \t";

  n_fix_header = sizeof(vcf_fix_headers) / sizeof(const char *);

  /* read a line */
  line = util_gzgets_line(vcf_fh);

  if(!line) {
    return -1;
  }
  
  cur = line;
  tok_num = 0;

  /* chrom */
  token = strsep(&cur, delim);
  if(token == NULL) {
    my_err("expected at least %d tokens per line\n", n_fix_header);
  }
  util_strncpy(vcf_info->chrom, token, sizeof(vcf_info->chrom));
  
  
  /* pos */
  token = strsep(&cur, delim);
  if(token == NULL) {
    my_err("expected at least %d tokens per line\n", n_fix_header);
  }
  vcf_info->pos = util_parse_long(token);
  
  /* ID */
  token = strsep(&cur, delim);
  if(token == NULL) {
    my_err("expected at least %d tokens per line\n", n_fix_header);
  }
  util_strncpy(vcf_info->id, token, sizeof(vcf_info->id));
  
  /* ref */
  token = strsep(&cur, delim);
  if(token == NULL) {
    my_err("expected at least %d tokens per line\n", n_fix_header);
  }
  vcf_info->ref_len = strlen(token);
  ref_len = util_strncpy(vcf_info->ref_allele, token,
			 sizeof(vcf_info->ref_allele));

  if(ref_len != vcf_info->ref_len) {
    my_warn("truncating long allele (%ld bp) to %ld bp\n",
	    vcf_info->ref_len, ref_len);
  }
  
  /* alt */
  token = strsep(&cur, delim);
  if(token == NULL) {
    my_err("expected at least %d tokens per line\n", n_fix_header);
  }
  vcf_info->alt_len = strlen(token);
  alt_len = util_strncpy(vcf_info->alt_allele, token,
			 sizeof(vcf_info->alt_allele));

  if(alt_len != vcf_info->alt_len) {
    my_warn("truncating long allele (%ld bp) to %ld bp\n",
	    vcf_info->alt_len, alt_len);
  }

  /* qual */
  token = strsep(&cur, delim);
  if(token == NULL) {
    my_err("expected at least %d tokens per line\n", n_fix_header);
  }
  util_strncpy(vcf_info->qual, token, sizeof(vcf_info->qual));

  /* filter */
  token = strsep(&cur, delim);
  if(token == NULL) {
    my_err("expected at least %d tokens per line\n", n_fix_header);
  }
  util_strncpy(vcf_info->filter, token, sizeof(vcf_info->filter));


  /* info */
  token = strsep(&cur, delim);
  if(token == NULL) {
    my_err("expected at least %d tokens per line\n", n_fix_header);
  }
  util_strncpy(vcf_info->info, token, sizeof(vcf_info->info));

  
  /* format */
  token = strsep(&cur, delim);
  if(token == NULL) {
    my_err("expected at least %d tokens per line\n", n_fix_header);
  }
  util_strncpy(vcf_info->format, token, sizeof(vcf_info->format));

  /* now parse haplotypes and/or genotype likelihoods */
  if(geno_probs && haplotypes) {
    char *cur_copy;    
    /* Both genotype probs and haplotypes requested.
     * Need to copy string because it is modified
     * by the tokenizing in the parsing functions.
     *
     * This could be made more efficient by doing the parsing
     * of both types of data at same time
     */
    cur_copy = my_malloc(strlen(cur)+1);
    strcpy(cur_copy, cur);
    
    parse_geno_probs(vcf_info, geno_probs, cur_copy);
    my_free(cur_copy);

    parse_haplotypes(vcf_info, haplotypes, cur);
  } else if(geno_probs) {
    parse_geno_probs(vcf_info, geno_probs, cur);
  } else if(haplotypes) {
    parse_haplotypes(vcf_info, haplotypes, cur);
  }
}
