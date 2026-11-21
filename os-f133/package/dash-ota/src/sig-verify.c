// SPDX-License-Identifier: MIT
// sig-verify PUBKEY SIG FILE: exit 0 only if SIG (64 bytes) is a valid Ed25519 signature of FILE by PUBKEY (32 raw
// bytes). The releases' SHA256SUMS.sig are made with OpenSSL's Ed25519 (the standard, SHA-512 one), so this checks
// with the same library: the image already carries libcrypto for the Android Auto receiver.
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>

static unsigned char *slurp(const char *path, long max, long *n)
{
	FILE *f = fopen(path, "rb");
	if (!f) return NULL;
	unsigned char *b = malloc((size_t)max + 1);
	*n = b ? (long)fread(b, 1, (size_t)max + 1, f) : -1;
	fclose(f);
	if (*n < 0 || *n > max) { free(b); return NULL; }
	return b;
}

int main(int argc, char **argv)
{
	if (argc != 4) { fprintf(stderr, "usage: sig-verify PUBKEY(32 raw bytes) SIG(64 bytes) FILE\n"); return 2; }
	long pn, sn, mn;
	unsigned char *pub = slurp(argv[1], 32, &pn), *sig = slurp(argv[2], 64, &sn), *msg = slurp(argv[3], 1 << 20, &mn);
	if (!pub || pn != 32 || !sig || sn != 64 || !msg) { fprintf(stderr, "sig-verify: cannot read key/signature/file (or wrong size)\n"); return 1; }
	EVP_PKEY *k = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, NULL, pub, 32);
	EVP_MD_CTX *c = EVP_MD_CTX_new();
	int ok = k && c && EVP_DigestVerifyInit(c, NULL, NULL, NULL, k) == 1 && EVP_DigestVerify(c, sig, 64, msg, (size_t)mn) == 1;
	EVP_MD_CTX_free(c);
	EVP_PKEY_free(k);
	if (!ok) { fprintf(stderr, "sig-verify: BAD signature for %s\n", argv[3]); return 1; }
	return 0;
}
