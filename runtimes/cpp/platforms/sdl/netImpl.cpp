/* Copyright 2013 David Axmark

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

	http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#include "config_platform.h"

#include <helpers/helpers.h>

#define NETWORKING_H
#include "networking.h"

#include "Syscall.h"

#include "fastevents.h"
#include "sdl_syscall.h"

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/tls1.h>

//***************************************************************************
//Helpers
//***************************************************************************

void ConnWaitEvent() {
	if(FE_WaitEvent(NULL) != 1) {
		LOGT("FE_WaitEvent failed");
		DEBIG_PHAT_ERROR;
	}
}
void ConnPushEvent(MAEvent* ep) {
	SDL_UserEvent event = { FE_ADD_EVENT, 0, ep, NULL };
	FE_PushEvent((SDL_Event*)&event);
}
void DefluxBinPushEvent(MAHandle handle, Stream& s) {
	SDL_UserEvent event = { FE_DEFLUX_BINARY, handle, &s, NULL };
	FE_PushEvent((SDL_Event*)&event);
}

//***************************************************************************
//SslConnection
//***************************************************************************

static void dumpSslErrors() {
	while(int err = ERR_get_error()) {
		char buf[1024];
		ERR_error_string_n(err, buf, sizeof(buf));
		LOG("%s\n", buf);
	}
}

#define TSSL_CUSTOM(func, test, action) { ssize_t _res = (size_t)(func); if(_res test) { IN_FILE_ON_LINE;\
	LOG("OpenSSL error %" PFSZT "\n", _res); dumpSslErrors(); action; } }

#define TSSL(func, test) TSSL_CUSTOM(func, test, return CONNERR_SSL)
#define TSSLZ(func) TSSL(func, == 0)
#define TSSLLTZ(func) TSSL(func, <= 0)

static SSL_CTX* sSslContext = NULL;

void MANetworkSslInit() {
	DEBUG_ASSERT(sSslContext == NULL);
	SSL_library_init();
	SSL_load_error_strings();
	const SSL_METHOD *sslmet = SSLv23_client_method();
	sSslContext = SSL_CTX_new((SSL_METHOD *)sslmet);
	TSSL_CUSTOM(sSslContext, == 0, DEBIG_PHAT_ERROR);
}

void MANetworkSslClose() {
	if(sSslContext != NULL) {
		SSL_CTX_free(sSslContext);
		sSslContext = NULL;
	}
}

SslConnection::~SslConnection() {
	close();
}

int SslConnection::connect() {
	TLTZ_PASS(TcpConnection::connect());
	TSSLZ(mSession = SSL_new(sSslContext));
	mState = eInit;
	TSSLLTZ(SSL_set_tlsext_host_name(mSession, mHostname.c_str()));
	TSSLZ(SSL_set_fd(mSession, mSock));
	TSSLLTZ(SSL_connect(mSession));
	mState = eHandshook;
	// TODO: Check that the CN matches the hostname.
	// TODO: Check the certificate chain against a set of root certificates.
#if 0
	X509* peerCert = SSL_get_peer_certificate(mSession);
	char commonName [512];
	X509_NAME* name = X509_get_subject_name(peerCert);
	X509_NAME_get_text_by_NID(name, NID_commonName, commonName, 512);
	if(stricmp(commonName, mHostname.c_str()) != 0) {
		LOG("Certificate was issued for '%s', but used for '%s'. Fail.\n");
		return CONNERR_SSL;
	}
#endif
	return 1;
}

int SslConnection::read(void* dst, int max) {
	int res = SSL_read(mSession, dst, max);
	if(res == 0) if(SSL_get_shutdown(mSession))
		return CONNERR_CLOSED;
	TSSLLTZ(res);
	return res;
}

int SslConnection::write(const void* src, int len) {
	TSSLLTZ(SSL_write(mSession, src, len));
	return 1;
}

void SslConnection::close() {
	if(mState == eHandshook) {
		TSSL_CUSTOM(SSL_shutdown(mSession), <0, );
		mState = eInit;
	}
	if(mState == eInit) {
		SSL_free(mSession);
	}
	mState = eIdle;
	TcpConnection::close();
}
