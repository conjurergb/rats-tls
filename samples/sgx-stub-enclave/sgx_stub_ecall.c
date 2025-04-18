/* Copyright (c) 2021 Intel Corporation
 * Copyright (c) 2020-2021 Alibaba Cloud
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <rats-tls/api.h>
#include <rats-tls/log.h>

#include "rats-tls/api.h"
#include "sgx_stub_t.h"

#include <time.h>

int ecall_rtls_server_startup(rats_tls_log_level_t log_level, char *attester_type,
			      char *verifier_type, char *tls_type, char *crypto_type,
			      unsigned long flags, uint32_t s_ip, uint16_t s_port)
{
	printf("=====ecall_rats_server_startup!\n");
	double init_begin, init_end, neg_begin, neg_end, rec_finish, trs_begin;
	ocall_current_time(&init_begin);

	rats_tls_conf_t conf;

	memset(&conf, 0, sizeof(conf));
	conf.log_level = log_level;
	snprintf(conf.attester_type, sizeof(conf.attester_type), "%s", attester_type);
	snprintf(conf.verifier_type, sizeof(conf.verifier_type), "%s", verifier_type);
	snprintf(conf.tls_type, sizeof(conf.tls_type), "%s", tls_type);
	snprintf(conf.crypto_type, sizeof(conf.crypto_type), "%s", crypto_type);
	conf.flags = flags;
	conf.cert_algo = RATS_TLS_CERT_ALGO_DEFAULT;

	/* Optional: Set some user-defined custom claims, which will be embedded in the certificate. */
	claim_t custom_claims[2] = {
		{ .name = "key_0", .value = (uint8_t *)"value_0", .value_size = sizeof("value_0") },
		{ .name = "key_1", .value = (uint8_t *)"value_1", .value_size = sizeof("value_1") },
	};
	conf.custom_claims = (claim_t *)custom_claims;
	conf.custom_claims_length = 2;

	int64_t sockfd;
	int sgx_status = ocall_socket(&sockfd, RTLS_AF_INET, RTLS_SOCK_STREAM, 0);
	if (sgx_status != SGX_SUCCESS || sockfd < 0) {
		RTLS_ERR("Failed to call socket() %#x %d\n", sgx_status, sockfd);
		return -1;
	}

	int reuse = 1;
	int ocall_ret = 0;
	sgx_status = ocall_setsockopt(&ocall_ret, sockfd, RTLS_SOL_SOCKET, RTLS_SO_REUSEADDR,
				      (const void *)&reuse, sizeof(int));
	if (sgx_status != SGX_SUCCESS || ocall_ret < 0) {
		RTLS_ERR("Failed to call setsockopt() %#x %d\n", sgx_status, ocall_ret);
		return -1;
	}

	/* Set keepalive options */
	int flag = 1;
	int tcp_keepalive_time = 30;
	int tcp_keepalive_intvl = 10;
	int tcp_keepalive_probes = 5;
	sgx_status = ocall_setsockopt(&ocall_ret, sockfd, RTLS_SOL_SOCKET, RTLS_SO_KEEPALIVE, &flag,
				      sizeof(flag));
	if (sgx_status != SGX_SUCCESS || ocall_ret < 0) {
		RTLS_ERR("Failed to call setsockopt() %#x %d\n", sgx_status, ocall_ret);
		return -1;
	}

	sgx_status = ocall_setsockopt(&ocall_ret, sockfd, RTLS_SOL_TCP, RTLS_TCP_KEEPIDLE,
				      &tcp_keepalive_time, sizeof(tcp_keepalive_time));
	if (sgx_status != SGX_SUCCESS || ocall_ret < 0) {
		RTLS_ERR("Failed to call setsockopt() %#x %d\n", sgx_status, ocall_ret);
		return -1;
	}

	sgx_status = ocall_setsockopt(&ocall_ret, sockfd, RTLS_SOL_TCP, RTLS_TCP_KEEPINTVL,
				      &tcp_keepalive_intvl, sizeof(tcp_keepalive_intvl));
	if (sgx_status != SGX_SUCCESS || ocall_ret < 0) {
		RTLS_ERR("Failed to call setsockopt() %#x %d\n", sgx_status, ocall_ret);
		return -1;
	}

	sgx_status = ocall_setsockopt(&ocall_ret, sockfd, RTLS_SOL_TCP, RTLS_TCP_KEEPCNT,
				      &tcp_keepalive_probes, sizeof(tcp_keepalive_probes));
	if (sgx_status != SGX_SUCCESS || ocall_ret < 0) {
		RTLS_ERR("Failed to call setsockopt() %#x %d\n", sgx_status, ocall_ret);
		return -1;
	}

	struct rtls_sockaddr_in s_addr;
	memset(&s_addr, 0, sizeof(s_addr));
	s_addr.sin_family = RTLS_AF_INET;
	s_addr.sin_addr.s_addr = s_ip;
	s_addr.sin_port = s_port;

	/* Bind the server socket */
	sgx_status = ocall_bind(&ocall_ret, sockfd, &s_addr, sizeof(s_addr));
	if (sgx_status != SGX_SUCCESS || ocall_ret == -1) {
		RTLS_ERR("Failed to call bind(), %#x %d\n", sgx_status, ocall_ret);
		return -1;
	}

	/* Listen for a new connection, allow 5 pending connections */
	sgx_status = ocall_listen(&ocall_ret, sockfd, 5);
	if (sgx_status != SGX_SUCCESS || ocall_ret == -1) {
		RTLS_ERR("Failed to call listen(), %#x %d\n", sgx_status, ocall_ret);
		return -1;
	}

	/* rats-tls init */
	librats_tls_init();
	rats_tls_handle handle;
	rats_tls_err_t ret = rats_tls_init(&conf, &handle);
	if (ret != RATS_TLS_ERR_NONE) {
		RTLS_ERR("Failed to initialize rats tls %#x\n", ret);
		return -1;
	}

	ret = rats_tls_set_verification_callback(&handle, NULL);
	if (ret != RATS_TLS_ERR_NONE) {
		RTLS_ERR("Failed to set verification callback %#x\n", ret);
		return -1;
	}

	/* Accept client connections */
	struct rtls_sockaddr_in c_addr;
	uint32_t addrlen_in = sizeof(c_addr);
	uint32_t addrlen_out;
	
	ocall_current_time(&init_end);

	while (1) {
		RTLS_INFO("Waiting for a connection from client ...\n");

		int64_t connd;
		sgx_status = ocall_accept(&connd, sockfd, &c_addr, addrlen_in, &addrlen_out);
		if (sgx_status != SGX_SUCCESS || connd < 0) {
			RTLS_ERR("Failed to call accept() %#x %d\n", sgx_status, connd);
			return -1;
		}
		
		ocall_current_time(&neg_begin);
		ret = rats_tls_negotiate(handle, connd);
		ocall_current_time(&neg_end);		

		if (ret != RATS_TLS_ERR_NONE) {
			RTLS_ERR("Failed to negotiate %#x\n", ret);
			goto err;
		}

		RTLS_DEBUG("Client connected successfully\n");

		char buf[256];
		size_t len = sizeof(buf);
		
		
		ret = rats_tls_receive(handle, buf, &len);
		ocall_current_time(&rec_finish);

		if (ret != RATS_TLS_ERR_NONE) {
			RTLS_ERR("Failed to receive %#x\n", ret);
			goto err;
		}

		if (len >= sizeof(buf))
			len = sizeof(buf) - 1;
		buf[len] = '\0';

		RTLS_INFO("Client: %s\n", buf);

		/* Reply back to the client */

		ocall_current_time(&trs_begin);
		ret = rats_tls_transmit(handle, buf, &len);
		if (ret != RATS_TLS_ERR_NONE) {
			RTLS_ERR("Failed to transmit %#x\n", ret);
			goto err;
		}

		ocall_close(&ocall_ret, connd);
		printf("[Server] init begin: 		%f\n", init_begin);
		printf("[Server] init end: 		%f\n", init_end);
		printf("[Server] negotiate begin: 	%f\n", neg_begin);
		printf("[Server] negotiate end: 	%f\n", neg_end);
		printf("[Server] receive finish:	%f\n", rec_finish);
		printf("[Server] transmit begin:	%f\n", trs_begin);

	}

	return 0;

err:
	/* Ignore the error code of cleanup in order to return the prepositional error */
	rats_tls_cleanup(handle);
	return -1;
}

int user_callback(void *args)
{
	rtls_evidence_t *ev = (rtls_evidence_t *)args;

	printf("verify_callback called, claims %p, claims_size %zu, args %p\n", ev->custom_claims,
	       ev->custom_claims_length, args);
	for (size_t i = 0; i < ev->custom_claims_length; ++i) {
		printf("custom_claims[%zu] -> name: '%s' value_size: %zu value: '%.*s'\n", i,
		       ev->custom_claims[i].name, ev->custom_claims[i].value_size,
		       (int)ev->custom_claims[i].value_size, ev->custom_claims[i].value);
	}
	return 1;
}

int ecall_rtls_client_startup(rats_tls_log_level_t log_level, char *attester_type,
			      char *verifier_type, char *tls_type, char *crypto_type,
			      unsigned long flags, uint32_t s_ip, uint16_t s_port, bool verdictd)
{
	rats_tls_conf_t conf;
	double init_begin, init_end, neg_begin, neg_end, rec_finish, trs_begin;
	ocall_current_time(&init_begin);
	
	printf("=====ecall_rats_client_startup!\n");
	
	memset(&conf, 0, sizeof(conf));
	conf.log_level = log_level;
	snprintf(conf.attester_type, sizeof(conf.attester_type), "%s", attester_type);
	snprintf(conf.verifier_type, sizeof(conf.verifier_type), "%s", verifier_type);
	snprintf(conf.tls_type, sizeof(conf.tls_type), "%s", tls_type);
	snprintf(conf.crypto_type, sizeof(conf.crypto_type), "%s", crypto_type);
	conf.flags = flags;
	conf.cert_algo = RATS_TLS_CERT_ALGO_DEFAULT;

	/* Create a socket that uses an internet IPv4 address,
	 * Sets the socket to be stream based (TCP),
	 * 0 means choose the default protocol.
	 */
	int64_t sockfd;
	int sgx_status = ocall_socket(&sockfd, RTLS_AF_INET, RTLS_SOCK_STREAM, 0);
	if (sgx_status != SGX_SUCCESS || sockfd < 0) {
		RTLS_ERR("Failed to call socket() %#x %d\n", sgx_status, sockfd);
		return -1;
	}

	struct rtls_sockaddr_in s_addr;
	memset(&s_addr, 0, sizeof(s_addr));
	s_addr.sin_family = RTLS_AF_INET;
	s_addr.sin_addr.s_addr = s_ip;
	s_addr.sin_port = s_port;

	/* Connect to the server */
	int ocall_ret = 0;
	sgx_status = ocall_connect(&ocall_ret, sockfd, &s_addr, sizeof(s_addr));
	if (sgx_status != SGX_SUCCESS || ocall_ret == -1) {
		RTLS_ERR("failed to call connect() %#x %d\n", sgx_status, ocall_ret);
		return -1;
	}

	/* rats-tls init */
	librats_tls_init();
	rats_tls_handle handle;
	rats_tls_err_t ret = rats_tls_init(&conf, &handle);
	if (ret != RATS_TLS_ERR_NONE) {
		RTLS_ERR("Failed to initialize rats tls %#x\n", ret);
		return -1;
	}

	ret = rats_tls_set_verification_callback(&handle, user_callback);
	if (ret != RATS_TLS_ERR_NONE) {
		RTLS_ERR("Failed to set verification callback %#x\n", ret);
		return -1;
	}
	ocall_current_time(&init_end);
	ocall_current_time(&neg_begin);
	ret = rats_tls_negotiate(handle, (int)sockfd);
	ocall_current_time(&neg_end);

	if (ret != RATS_TLS_ERR_NONE) {
		RTLS_ERR("Failed to negotiate %#x\n", ret);
		goto err;
	}

	const char *msg;
	if (verdictd)
		msg = "{ \"command\": \"echo\", \"data\": \"Hello and welcome to RATS-TLS!\\n\" }";
	else
		msg = "\033[94mHello and welcome to RATS-TLS!\033[0m\n";

	size_t len = strlen(msg);
	ocall_current_time(&trs_begin);
	ret = rats_tls_transmit(handle, (void *)msg, &len);
	if (ret != RATS_TLS_ERR_NONE || len != strlen(msg)) {
		RTLS_ERR("Failed to transmit %#x\n", ret);
		goto err;
	}

	char buf[256];
	len = sizeof(buf);
	ret = rats_tls_receive(handle, buf, &len);
	ocall_current_time(&rec_finish);
	if (ret != RATS_TLS_ERR_NONE) {
		RTLS_ERR("Failed to receive %#x\n", ret);
		goto err;
	}

	if (len >= sizeof(buf))
		len = sizeof(buf) - 1;
	buf[len] = '\0';

	/* Server running in SGX Enclave will send mrenclave, mrsigner and hello message to client */
	if (len >= 2 * sizeof(sgx_measurement_t)) {
		RTLS_INFO("Server's SGX identity:\n");
		RTLS_INFO("  . MRENCLAVE = ");
		for (int i = 0; i < 32; ++i)
			printf("%02x", (uint8_t)buf[i]);
		printf("\n");
		RTLS_INFO("  . MRSIGNER  = ");
		for (int i = 32; i < 64; ++i)
			printf("%02x", (uint8_t)buf[i]);
		printf("\n");

		memcpy(buf, buf + 2 * sizeof(sgx_measurement_t),
		       len - 2 * sizeof(sgx_measurement_t));
		buf[len - 2 * sizeof(sgx_measurement_t)] = '\0';

		RTLS_INFO("Server:\n%s\n", buf);
	} else {
		/* Server not running in SGX Enlcave will only send hello message to client */
		RTLS_INFO("Server: %s\n", buf);
	}

	if (verdictd)
		msg = "\033[94mHello and welcome to RATS-TLS!\033[0m\n";

	/* Sanity check whether the response is expected */
	if (strcmp(msg, buf)) {
		printf("Invalid response retrieved from rats-tls server\n");
		goto err;
	}
	printf("[Client] init begin: 		%f\n", init_begin);
	printf("[Client] init end: 		%f\n", init_end);
	printf("[Client] negotiate begin: 	%f\n", neg_begin);
	printf("[Client] negotiate end: 	%f\n", neg_end);
	printf("[Client] receive finish:	%f\n", rec_finish);
	printf("[Client] transmit begin:	%f\n", trs_begin);

	ret = rats_tls_cleanup(handle);
	if (ret != RATS_TLS_ERR_NONE)
		RTLS_ERR("Failed to cleanup %#x\n", ret);

	return ret;

err:
	/* Ignore the error code of cleanup in order to return the prepositional error */
	rats_tls_cleanup(handle);
	return -1;
}
