#include <synth/crypto.h>

#include "win32.h"

#define LDAP_UNAVAILABLE 0x34

static uint32_t ldap_unavailable(void)
{
    return LDAP_UNAVAILABLE;
}

static const char *ldap_err2string(uint32_t err)
{
    UNUSED(err);
    return "LDAP is not available on SynthOS";
}

static const struct w32_export wldap32_exports[] = {
    W32_AS("ldap_init", w32_zero0),
    W32_AS("ldap_initA", w32_zero0),
    W32_AS("ldap_initW", w32_zero0),
    W32_AS("ldap_sslinit", w32_zero0),
    W32_AS("ldap_sslinitA", w32_zero0),
    W32_AS("ldap_sslinitW", w32_zero0),
    W32_AS("ldap_open", w32_zero0),
    W32_AS("ldap_set_option", ldap_unavailable),
    W32_AS("ldap_get_option", ldap_unavailable),
    W32_AS("ldap_bind_s", ldap_unavailable),
    W32_AS("ldap_simple_bind_s", ldap_unavailable),
    W32_AS("ldap_search_s", ldap_unavailable),
    W32_AS("ldap_unbind_s", ldap_unavailable),
    W32_AS("ldap_unbind", ldap_unavailable),
    W32_AS("ldap_err2string", ldap_err2string),
    W32_AS("ldap_first_entry", w32_zero0),
    W32_AS("ldap_next_entry", w32_zero0),
    W32_AS("ldap_first_attribute", w32_zero0),
    W32_AS("ldap_next_attribute", w32_zero0),
    W32_AS("ldap_get_dn", w32_zero0),
    W32_AS("ldap_get_values_len", w32_zero0),
    W32_AS("ldap_value_free_len", w32_zero0),
    W32_AS("ldap_memfree", w32_zero0),
    W32_AS("ldap_msgfree", w32_zero0),
    W32_AS("ber_free", w32_zero0),
};

static void *const sspi_table[] = {
    (void *)1,         w32_secerr2,  NULL,         w32_secerr3, w32_secerr9,  w32_secerr1, NULL,
    w32_secerr12,      w32_secerr9,  w32_secerr2,  w32_secerr1, w32_secerr2,  w32_secerr3, w32_secerr1,
    w32_secerr1,       w32_secerr4,  w32_secerr4,  w32_zero1,   w32_secerr2,  NULL,        NULL,
    w32_secerr4,       w32_secerr4,  w32_secerr8,  NULL,        w32_secerr2,  w32_secerr4, w32_secerr4,
    w32_secerr4,       w32_secerr4,  w32_secerr8,  w32_secerr4, w32_secerr4,
};

static const void *WINAPI InitSecurityInterface(void)
{
    return sspi_table;
}

static const struct w32_export secur32_exports[] = {
    W32_AS("DeleteSecurityContext", w32_secerr1), W32_AS("FreeCredentialsHandle", w32_secerr1),
    W32_AS("QueryContextAttributesW", w32_secerr3), W32_AS("LsaConnectUntrusted", w32_secerr1),
    W32_AS("LsaDeregisterLogonProcess", w32_secerr1), W32_AS("LsaFreeReturnBuffer", w32_secerr1),
    W32_AS("LsaLogonUser", w32_secerr14),
    W32_AS("InitSecurityInterfaceA", InitSecurityInterface),
    W32_AS("InitSecurityInterfaceW", InitSecurityInterface),
    W32_AS("FreeContextBuffer", w32_zero1),
    W32_AS("AcquireCredentialsHandleA", w32_secerr9),
    W32_AS("AcquireCredentialsHandleW", w32_secerr9),
    W32_AS("InitializeSecurityContextA", w32_secerr12),
    W32_AS("InitializeSecurityContextW", w32_secerr12),
    W32_AS("QuerySecurityPackageInfoA", w32_secerr2),
    W32_AS("QuerySecurityPackageInfoW", w32_secerr2),
};

static const struct w32_export bcrypt_extra_exports[] = {
    W32_AS("BCryptDeriveKey", w32_status7),
    W32_AS("BCryptDestroySecret", w32_zero1),
    W32_AS("BCryptFinalizeKeyPair", w32_status2),
    W32_AS("BCryptGenerateKeyPair", w32_status4),
    W32_AS("BCryptImportKeyPair", w32_status7),
    W32_AS("BCryptSecretAgreement", w32_status4),
    W32_AS("BCryptSignHash", w32_status8),
    W32_AS("BCryptVerifySignature", w32_status7),
};

static const struct w32_export crypt32_extra_exports[] = {
    W32_AS("CertAddEncodedCertificateToStore", w32_fail6),
    W32_AS("CertCreateCertificateChainEngine", w32_fail2),
    W32_AS("CertFindChainInStore", w32_fail6),
    W32_AS("CertGetCertificateContextProperty", w32_fail4),
    W32_AS("CertGetIntendedKeyUsage", w32_fail4),
    W32_AS("CryptAcquireCertificatePrivateKey", w32_fail6),
    W32_AS("CryptMsgGetParam", w32_fail5),
    W32_AS("CryptVerifyCertificateSignatureEx", w32_fail8),
    W32_AS("CertFindExtension", w32_zero3),
    W32_AS("CertFreeCertificateChainEngine", w32_zero1),
    W32_AS("CertGetNameStringA", w32_one6),
    W32_AS("CertGetNameStringW", w32_one6),
    W32_AS("CryptDecodeObjectEx", w32_fail8),
    W32_AS("CryptQueryObject", w32_fail11),
    W32_AS("PFXImportCertStore", w32_fail3),
};

static const struct w32_export advapi_crypt_exports[] = {
    W32_AS("CryptDestroyKey", w32_one1),
    W32_AS("CryptEncrypt", w32_fail7),
    W32_AS("CryptDecrypt", w32_fail6),
    W32_AS("CryptImportKey", w32_fail6),
};

static BOOL WINAPI ProcessPrng(void *buf, SIZE_T len)
{
    random_bytes(buf, len);
    return TRUE;
}

static const struct w32_export bcryptprimitives_exports[] = {
    W32_FN(ProcessPrng),
};

W32_EXPORTS(wldap32_module, "wldap32.dll", wldap32_exports);
W32_EXPORTS(bcryptprimitives_module, "bcryptprimitives.dll", bcryptprimitives_exports);
W32_EXPORTS(secur32_module, "secur32.dll", secur32_exports);
W32_EXPORTS(bcrypt_extra_module, "bcrypt.dll", bcrypt_extra_exports);
W32_EXPORTS(crypt32_extra_module, "crypt32.dll", crypt32_extra_exports);
W32_EXPORTS(advapi_crypt_module, "advapi32.dll", advapi_crypt_exports);
