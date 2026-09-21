add_module(XRay.Core.Crypto
  TYPE STATIC

  LINKS
  FastDynamicCast
  OpenSSL

  XRay.Core.Defines
  
  XRay.Core.Includes

  DEFINES
  CRYPTO_BUILD

  INCLUDES
  "${CMAKE_CURRENT_SOURCE_DIR}/../3rd party/crypto"

  SOURCES
  "../3rd party/crypto/crypto.cpp"
  "../3rd party/crypto/crypto.h"
  "../3rd party/crypto/crypto_api_def.h"
  "../3rd party/crypto/xr_dsa.cpp"
  "../3rd party/crypto/xr_dsa.h"
  "../3rd party/crypto/xr_sha.cpp"
  "../3rd party/crypto/xr_sha.h"
)