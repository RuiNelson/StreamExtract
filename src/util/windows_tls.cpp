#include "windows_tls.hpp"

#include <mbedtls/threading.h>
#include <wincrypt.h>

#include <stdexcept>

namespace streamextract {
namespace {
void mutex_init(mbedtls_threading_mutex_t* mutex) {
  InitializeSRWLock(&mutex->lock);
  mutex->initialized = 1;
}

void mutex_free(mbedtls_threading_mutex_t* mutex) { mutex->initialized = 0; }

int mutex_lock(mbedtls_threading_mutex_t* mutex) {
  if (mutex == nullptr || mutex->initialized == 0) {
    return MBEDTLS_ERR_THREADING_BAD_INPUT_DATA;
  }
  AcquireSRWLockExclusive(&mutex->lock);
  return 0;
}

int mutex_unlock(mbedtls_threading_mutex_t* mutex) {
  if (mutex == nullptr || mutex->initialized == 0) {
    return MBEDTLS_ERR_THREADING_BAD_INPUT_DATA;
  }
  ReleaseSRWLockExclusive(&mutex->lock);
  return 0;
}
}  // namespace

void windows_tls_init() {
  mbedtls_threading_set_alt(mutex_init, mutex_free, mutex_lock, mutex_unlock);
}

std::string windows_ca_bundle() {
  // mbed TLS verifies the connection; Windows only supplies the trusted roots.
  const HCERTSTORE store = CertOpenSystemStoreW(0, L"ROOT");
  if (store == nullptr) {
    throw std::runtime_error("cannot open the Windows root certificate store");
  }
  std::string bundle;
  PCCERT_CONTEXT cert = nullptr;
  try {
    while ((cert = CertEnumCertificatesInStore(store, cert)) != nullptr) {
      DWORD size = 0;
      if (!CryptBinaryToStringA(cert->pbCertEncoded, cert->cbCertEncoded, CRYPT_STRING_BASE64HEADER,
                               nullptr, &size)) {
        throw std::runtime_error("cannot read a Windows root certificate");
      }
      std::string pem(size, '\0');
      if (!CryptBinaryToStringA(cert->pbCertEncoded, cert->cbCertEncoded, CRYPT_STRING_BASE64HEADER,
                               pem.data(), &size)) {
        throw std::runtime_error("cannot convert a Windows root certificate to PEM");
      }
      pem.resize(size);  // The second call excludes the terminating NUL from size.
      bundle += pem;
    }
  } catch (...) {
    if (cert != nullptr) {
      CertFreeCertificateContext(cert);
    }
    CertCloseStore(store, 0);
    throw;
  }
  CertCloseStore(store, 0);
  if (bundle.empty()) {
    throw std::runtime_error("the Windows root certificate store is empty; supply --cacert");
  }
  return bundle;
}
}  // namespace streamextract
