/*
 * This file is part of GyroidOS
 * Copyright(c) 2013 - 2017 Fraunhofer AISEC
 * Fraunhofer-Gesellschaft zur Förderung der angewandten Forschung e.V.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 (GPL 2), as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GPL 2 license for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, see <http://www.gnu.org/licenses/>
 *
 * The full GNU General Public License is included in this distribution in
 * the file called "COPYING".
 *
 * Contact Information:
 * Fraunhofer AISEC <gyroidos@aisec.fraunhofer.de>
 */

#ifdef ANDROID
#include "device/fraunhofer/common/cml/tpm2_control/tpm2d.pb-c.h"
#else
#include "tpm2d.pb-c.h"
#endif

#include "tpm2d_shared.h"

#include "common/macro.h"
#include "common/mem.h"
#include "common/protobuf.h"
#include "common/protobuf-text.h"
#include "common/sock.h"
#include "common/file.h"

#include <getopt.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/stat.h>

static void
print_usage(const char *cmd)
{
	printf("\n");
	printf("Usage: %s [-s <socket file>] <command> [<command args>]\n", cmd);
	printf("\n");
	printf("commands:\n");
	printf("\tdmcrypt_setup [-l|--key_len <len>] <device path> [<passwd>]\n"
	       "\t\tSetup device mapper with tpm2d's internal disk encryption key,\n"
	       "\t\tpassword for corresponding nvindex,\n"
	       "\t\tif -l is set, use len bytes of nvindex as key\n");
	printf("\tdmcrypt_lock <passwd>\n"
	       "\t\tLocks further dmsetup attampts by locking tpm2d's internal disk encryption key,\n"
	       "\t\tpassword for corresponding nvindex\n");
	printf("\texit\n\t\tStop TPM2D daemon\n");
	printf("\tgetrandom <size>\n\t\tRequest some random data of size size from TPM\n");
	printf("\tclear <passwd>\n\t\tClear TPM using lockout password\n");
	printf("\tchange_owner <passwd> <new passwd>\n"
	       "\t\tChanges the password for the owner hierarchy of the TPM\n");
	printf("\n");
	exit(-1);
}

static void
send_message(const char *socket_file, ControllerToTpm *msg, bool has_response)
{
	// send message
	protobuf_dump_message(STDOUT_FILENO, (ProtobufCMessage *)msg);
	int sock = sock_unix_create_and_connect(SOCK_STREAM, socket_file);
	if (sock < 0) {
		exit(-3);
	}
	ssize_t msg_size = protobuf_send_message(sock, (ProtobufCMessage *)msg);
	if (msg_size < 0) {
		exit(-4);
	}
	// recv response if applicable
	// TODO for now just dump the response in text format
	if (has_response) {
		TpmToController *resp = (TpmToController *)protobuf_recv_message(
			sock, &tpm_to_controller__descriptor);
		if (!resp) {
			exit(-5);
		}
		DEBUG("Got Response from TPM2Controller");
		protobuf_dump_message(STDOUT_FILENO, (ProtobufCMessage *)resp);
		protobuf_free_message((ProtobufCMessage *)resp);
	}

	shutdown(sock, SHUT_RDWR);
	close(sock);
}

static const struct option global_options[] = { { "socket", required_argument, 0, 's' },
						{ "help", no_argument, 0, 'h' },
						{ 0, 0, 0, 0 } };

static const struct option dmsetup_options[] = { { "key_len", required_argument, 0, 'l' },
						 { 0, 0, 0, 0 } };

static ControllerToTpm__FdeKeyType
get_fde_key_type(int len)
{
	INFO("Get FdeKeyType for len: %d", len);

	switch (len) {
	case 32:
		return CONTROLLER_TO_TPM__FDE_KEY_TYPE__XTS_AES128;
	case 48:
		return CONTROLLER_TO_TPM__FDE_KEY_TYPE__XTS_AES192;
	case 64:
		return CONTROLLER_TO_TPM__FDE_KEY_TYPE__XTS_AES256;
	default:
		INFO("Unsupported len %d for FdeKeyType, using default (XTS-AES256)", len);
		return CONTROLLER_TO_TPM__FDE_KEY_TYPE__XTS_AES256;
	}
}

int
main(int argc, char *argv[])
{
	logf_register(&logf_test_write, stderr);

	bool has_response = false;
	const char *socket_file = TPM2D_SOCKET;
	for (int c, option_index = 0;
	     - 1 != (c = getopt_long(argc, argv, "+s:h", global_options, &option_index));) {
		switch (c) {
		case 's':
			socket_file = optarg;
			break;
		default: // includes cases 'h' and '?'
			print_usage(argv[0]);
		}
	}

	if (!file_exists(socket_file)) {
		WARN("Could not find socket file %s. Aborting.\n", socket_file);
		exit(-2);
	}

	// need at least one more argument (i.e. command string)
	if (optind >= argc) {
		INFO("need at least one more argument (i.e. command string)");
		print_usage(argv[0]);
	}

	// build ControllerToTpm message
	ControllerToTpm msg = CONTROLLER_TO_TPM__INIT;

	const char *command = argv[optind++];
	if (!strcasecmp(command, "dmcrypt_setup")) {
		has_response = true;
		msg.code = CONTROLLER_TO_TPM__CODE__DMCRYPT_SETUP;
		if (optind >= argc)
			print_usage(argv[0]);

		optind--;
		char **dm_argv = &argv[optind];
		int dm_argc = argc - optind;
		optind = 0; // reset optind to scan command-specific options
		for (int c, option_index = 0;
		     - 1 !=
		     (c = getopt_long(dm_argc, dm_argv, "+l:", dmsetup_options, &option_index));) {
			switch (c) {
			case 'l':
				msg.has_dmcrypt_key_type = true;
				msg.dmcrypt_key_type = get_fde_key_type(atoi(optarg));
				break;
			default:
				print_usage(argv[0]);
				ASSERT(false); // never reached
			}
		}
		optind += argc - dm_argc; // adjust optind to be used with argv

		msg.dmcrypt_device = argv[optind++];
		if (optind < argc)
			msg.password = argv[optind++];

		DEBUG("Sending DMCRYPT_SETUP command TPM");
		goto send_message;
	}
	if (!strcasecmp(command, "exit")) {
		msg.code = CONTROLLER_TO_TPM__CODE__EXIT;
		DEBUG("Sending EXIT command to TPM2D");
		goto send_message;
	}
	if (!strcasecmp(command, "getrandom")) {
		has_response = true;
		msg.code = CONTROLLER_TO_TPM__CODE__RANDOM_REQ;
		if (optind >= argc)
			print_usage(argv[0]);

		msg.has_rand_size = true;
		msg.rand_size = atoi(argv[optind++]);

		DEBUG("Sending GETRANDOM command TPM");
		goto send_message;
	}
	if (!strcasecmp(command, "clear")) {
		has_response = true;
		msg.code = CONTROLLER_TO_TPM__CODE__CLEAR;
		if (optind < argc)
			msg.password = argv[optind++];

		DEBUG("Sending CLEAR command to TPM");
		goto send_message;
	}
	if (!strcasecmp(command, "dmcrypt_lock")) {
		has_response = true;
		msg.code = CONTROLLER_TO_TPM__CODE__DMCRYPT_LOCK;
		if (optind < argc)
			msg.password = argv[optind++];

		DEBUG("Sending DMCRYPT_LOCK command to TPM");
		goto send_message;
	}
	if (!strcasecmp(command, "change_owner")) {
		has_response = true;
		msg.code = CONTROLLER_TO_TPM__CODE__CHANGE_OWNER_PWD;
		if (optind < argc)
			msg.password = argv[optind++];

		if (optind < argc)
			msg.password_new = argv[optind++];

		DEBUG("Sending CHNAGE_OWNER_PWD command TPM");
		goto send_message;
	}
	if (!strcasecmp(command, "dmcrypt_reset")) {
		has_response = true;
		msg.code = CONTROLLER_TO_TPM__CODE__DMCRYPT_RESET;
		if (optind < argc)
			msg.password = argv[optind++];

		DEBUG("Sending DMCRYPT_REST command TPM");
		goto send_message;
	}

send_message:
	send_message(socket_file, &msg, has_response);

	return 0;
}
