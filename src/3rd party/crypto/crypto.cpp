#include "crypto.h"
#include <openssl/bn.h>
#include <openssl/rand.h>
#include <openssl/dsa.h>

#pragma comment(lib, "xrCore.lib")

namespace crypto
{

#if (OPENSSL_VERSION_NUMBER >= 0x10100000L)
static void* openssl_malloc(size_t size, const char* file, int line)
{
	return xr_malloc(size);
}
static void* openssl_realloc(void* ptr, size_t size, const char* file, int line)
{
	return xr_realloc(ptr, size);
}
static void openssl_free(void* ptr, const char* file, int line)
{
	xr_free(ptr);
}
#else
static void unsafe_xr_free(void* ptr)
{
	xr_free(ptr);
};
#endif

static unsigned char rnd_seed[] = "S.T.A.L.K.E.R. 4ever Rulezz !!!";

CRYPTO_API	void		xr_crypto_init	()
{
	string256 random_string;
	xr_sprintf					(random_string, "%I64d_%s", CPU::QPC(), rnd_seed);
	//sprintf_s					(random_string, "%s", rnd_seed);
#if (OPENSSL_VERSION_NUMBER >= 0x10100000L)
	CRYPTO_set_mem_functions	(openssl_malloc, openssl_realloc, openssl_free);
#else
	CRYPTO_set_mem_functions	(xr_malloc, xr_realloc, unsafe_xr_free);
#endif
	RAND_seed					(random_string, xr_strlen(random_string));
	//unsigned int siglen;
	//tmp_dsa_params->flags |= DSA_FLAG_NO_EXP_CONSTTIME;
	//ZeroMemory(sig, sizeof(sig));
	//DSA_sign(0, digest, sizeof(digest), sig, &siglen, tmp_dsa_params);
	//DSA_verify(0, digest, sizeof(digest), sig, siglen, tmp_dsa_params);
/*#ifdef DEBUG
	CRYPTO_dbg_set_options		(V_CRYPTO_MDEBUG_ALL);
	CRYPTO_mem_ctrl				(CRYPTO_MEM_CHECK_ON);
#endif*/
}

} //namespace crypto