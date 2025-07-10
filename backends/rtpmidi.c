#define BACKEND_NAME "rtpmidi"
//#define DEBUG

#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>

//mmbackend pulls in windows.h, required before more specific includes
#include "libmmbackend.h"
#include "rtpmidi.h"

#ifdef _WIN32
#include <iphlpapi.h>
#else
#include <arpa/inet.h>
#include <net/if.h>
#include <sys/types.h>
#include <ifaddrs.h>
#endif

//TODO learn peer ssrcs
//TODO default mode?
//TODO internal loop mode
//TODO for some reason, the announce packet generates an exception in the wireshark dns dissector
//TODO rename and document most functions
//TODO timeout non-responsive peers (connected = 0) to allow discovery to reconnect them

/*
 * CAVEAT EMPTOR: This is one of the largest backends yet, due to the
 * sheer number of protocols involved and their respective complexity.
 * The following RFCs may be useful for understanding this backend:
 * 	* RFC 6295 (MIDI Payload for RTP)
 * 	* RFC 1035 (DNS)
 * 	* RFC 6762 (mDNS)
 * 	* RFC 6763 (DNS Service Discovery)
 * 	* RFC 2782 (SRV RR for DNS)
 * 	* To a lesser extent, RFC3550 (RTP)
 * Additionally, a strong understanding of the MIDI data stream as well as the details of multicast
 * networking for IPv4 and IPv6 are very helpful.
 */

static struct /*_rtpmidi_global*/ {
	//mdns is split into v6 and v4 to avoid having to translate ipv6-mapped-ipv4 source addresses
	int mdns_fd;
	int mdns4_fd;

	char* mdns_name;
	char* mdns_interface;
	#ifdef _WIN32
	unsigned mdns_adapter;
	unsigned mdns6_adapter;
	#endif

	uint8_t detect;
	uint64_t last_service;

	size_t addresses;
	rtpmidi_addr* address;

	size_t invites;
	rtpmidi_invite* invite;
} cfg = {
	.mdns_fd = -1,
	.mdns4_fd = -1,
	.mdns_name = NULL,
	.mdns_interface = NULL,

	.detect = 0,
	.last_service = 0,

	.addresses = 0,
	.address = NULL,

	.invites = 0,
	.invite = NULL
};

MM_PLUGIN_API int init(){
	backend rtpmidi = {
		.name = BACKEND_NAME,
		.conf = rtpmidi_configure,
		.create = rtpmidi_instance,
		.conf_instance = rtpmidi_configure_instance,
		.channel = rtpmidi_channel,
		.handle = rtpmidi_set,
		.interval = rtpmidi_interval,
		.process = rtpmidi_handle,
		.start = rtpmidi_start,
		.shutdown = rtpmidi_shutdown
	};

	if(sizeof(rtpmidi_channel_ident) != sizeof(uint64_t)){
		LOG("Channel identification union out of bounds");
		return 1;
	}

	if(mm_backend_register(rtpmidi)){
		LOG("Failed to register backend");
		return 1;
	}

	return 0;
}

static int dns_decode_name(uint8_t* buffer, size_t len, size_t start, dns_name* out){
	size_t offset = 0, output_offset = 0;
	uint8_t current_label = 0;
	uint16_t ptr_target = 0;

	//reset output data length and terminate null name
	out->length = 0;
	if(out->name){
		out->name[0] = 0;
	}

	while(start + offset < len){
		current_label = buffer[start + offset];

		//if we're at a pointer, move there and stop counting data length
		if(DNS_POINTER(current_label)){
			if(start + offset + 1 >= len){
				LOG("mDNS internal pointer out of bounds");
				return 1;
			}

			//do this before setting the target
			if(!ptr_target){
				out->length += 2;
			}

			//calculate pointer target
			ptr_target = DNS_LABEL_LENGTH(current_label) << 8 | buffer[start + offset + 1];

			if(ptr_target >= len){
				LOG("mDNS internal pointer target out of bounds");
				return 1;
			}
			start = ptr_target;
			offset = 0;
		}
		else{
			if(DNS_LABEL_LENGTH(current_label) == 0){
				if(!ptr_target){
					out->length++;
				}
				break;
			}

			//check whether we have the bytes we need
			if(start + offset + DNS_LABEL_LENGTH(current_label) > len){
				LOG("mDNS bytes missing");
				return 1;
			}

			//check whether we have space in the output
			if(output_offset + DNS_LABEL_LENGTH(current_label) > out->alloc){
				out->name = realloc(out->name, (output_offset + DNS_LABEL_LENGTH(current_label) + 2) * sizeof(uint8_t));
				if(!out->name){
					LOG("Failed to allocate memory");
					return 1;
				}
				out->alloc = output_offset + DNS_LABEL_LENGTH(current_label);
			}

			//copy data from this label to output buffer
			memcpy(out->name + output_offset, buffer + start + offset + 1, DNS_LABEL_LENGTH(current_label));
			output_offset += DNS_LABEL_LENGTH(current_label) + 1;
			offset += DNS_LABEL_LENGTH(current_label) + 1;
			out->name[output_offset - 1] = '.';
			out->name[output_offset] = 0;
			if(!ptr_target){
				out->length = offset;
			}
		}
	}
	return 0;
}

static int dns_encode_name(char* name, dns_name* out){
	char* save = NULL, *token = NULL;
	out->length = 0;

	for(token = strtok_r(name, ".", &save); token; token = strtok_r(NULL, ".", &save)){
		//make space for this label, its length and a trailing root label
		if(out->alloc < out->length + strlen(token) + 1 + 1 || !out->name){
			out->name = realloc(out->name, (out->length + strlen(token) + 2) * sizeof(char));
			if(!out->name){
				LOG("Failed to allocate memory");
				return 1;
			}
			out->alloc = out->length + strlen(token) + 2;
		}
		//FIXME check label length before adding
		out->name[out->length] = strlen(token);
		memcpy(out->name + out->length + 1, token, strlen(token));
		out->length += strlen(token) + 1;
	}

	//last-effort allocate a root buffer
	if(!out->alloc){
		out->name = calloc(1, sizeof(char));
		if(!out->name){
			LOG("Failed to allocate memory");
			return 1;
		}
		out->alloc = 1;
	}

	//add root label
	out->name[out->length] = 0;
	out->length++;

	return 0;
}

static ssize_t dns_push_rr(uint8_t* buffer, size_t length, dns_rr** out, char* name, uint16_t type, uint16_t class, uint32_t ttl, uint16_t len){
	dns_rr* rr = NULL;
	size_t offset = 0;
	dns_name encode = {
		.alloc = 0
	};

	//if requested, encode name
	if(name && dns_encode_name(name, &encode)){
		LOGPF("Failed to encode DNS name %s", name);
		goto bail;
	}

	if(encode.length + sizeof(dns_rr) > length){
		LOGPF("Failed to encode DNS name %s, insufficient space", name);
		goto bail;
	}

	if(name){
		//copy encoded name to buffer
		memcpy(buffer, encode.name, encode.length);
		offset += encode.length;
	}

	rr = (dns_rr*) (buffer + offset);
	rr->rtype = htobe16(type);
	rr->rclass = htobe16(class);
	rr->ttl = htobe32(ttl);
	rr->data = htobe16(len);
	offset += sizeof(dns_rr);
	if(out){
		*out = rr;
	}

	free(encode.name);
	return offset;

bail:
	free(encode.name);
	return -1;
}

static int rtpmidi_announce_addrs(){
	char repr[INET6_ADDRSTRLEN + 1] = "", iface[2048] = "";
	union {
		struct sockaddr_in* in4;
		struct sockaddr_in6* in6;
		struct sockaddr* in;
	} addr;

	#ifdef _WIN32
	IP_ADAPTER_UNICAST_ADDRESS_LH* unicast_addr = NULL;
	IP_ADAPTER_ADDRESSES addrs[250] , *iter = NULL;
	size_t bytes_alloc = sizeof(addrs);

	unsigned long status = GetAdaptersAddresses(0, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
			NULL, addrs, (unsigned long*) &bytes_alloc);
	if(status != ERROR_SUCCESS){
		//FIXME might try to resize the result list and retry at some point...
		FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, status,
				MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), iface, sizeof(iface), NULL);
		LOGPF("Failed to query local interface addresses (%lu): %s", status, iface);
		return 1;
	}

	for(iter = addrs; iter; iter = iter->Next){
		//friendlyname is a wide string, print it into interface for basic conversion and to avoid implementing wide string handling
		snprintf(iface, sizeof(iface), "%S", iter->FriendlyName);
		//filter interfaces if requested
		if(cfg.mdns_interface && strncmp(iface, cfg.mdns_interface, min(strlen(iface), strlen(cfg.mdns_interface)))){
			continue;
		}

		//for exact matches, use exactly this interface for multicasts
		if(!strcmp(iface, cfg.mdns_interface)){
			LOGPF("Using interface %s for mDNS discovery", iface);
			cfg.mdns_adapter = iter->IfIndex;
			cfg.mdns6_adapter = iter->Ipv6IfIndex;
		}

		for(unicast_addr = (IP_ADAPTER_UNICAST_ADDRESS_LH*) iter->FirstUnicastAddress; unicast_addr; unicast_addr = unicast_addr->Next){
			addr.in = unicast_addr->Address.lpSockaddr;
	#else
	struct ifaddrs* ifa = NULL, *iter = NULL;

	if(getifaddrs(&ifa)){
		LOGPF("Failed to get adapter address information: %s", mmbackend_socket_strerror(errno));
		return 1;
	}

	for(iter = ifa; iter; iter = iter->ifa_next){
		if((!cfg.mdns_interface || !strcmp(cfg.mdns_interface, iter->ifa_name))
					&& strcmp(iter->ifa_name, "lo")
					&& iter->ifa_addr){
			snprintf(iface, sizeof(iface), "%s", iter->ifa_name);
			addr.in = iter->ifa_addr;
	#endif
			if(addr.in->sa_family != AF_INET && addr.in->sa_family != AF_INET6){
				continue;
			}

			cfg.address = realloc(cfg.address, (cfg.addresses + 1) * sizeof(rtpmidi_addr));
			if(!cfg.address){
				cfg.addresses = 0;
				LOG("Failed to allocate memory");
				return 1;
			}

			cfg.address[cfg.addresses].family = addr.in->sa_family;
			memcpy(&cfg.address[cfg.addresses].addr,
					(addr.in->sa_family == AF_INET) ? (void*) &addr.in4->sin_addr.s_addr : (void*) &addr.in6->sin6_addr.s6_addr,
					(addr.in->sa_family == AF_INET) ? 4 : 16);

			LOGPF("mDNS announce address %" PRIsize_t ": %s (from %s)", cfg.addresses, mmbackend_sockaddr_ntop(addr.in, repr, sizeof(repr)), iface);
			cfg.addresses++;
		}
	}

	#ifndef _WIN32
	freeifaddrs(ifa);
	#endif

	if(!cfg.addresses){
		LOG("Failed to gather local IP addresses for mDNS announce");
		return 1;
	}
	return 0;
}

static uint32_t rtpmidi_interval(){
	return max(0, (int64_t) RTPMIDI_SERVICE_INTERVAL - (int64_t) (mm_timestamp() - cfg.last_service));
}

static int rtpmidi_configure(char* option, char* value){
	if(!strcmp(option, "mdns-name")){
		if(cfg.mdns_name){
			LOG("Duplicate mdns-name assignment");
			return 1;
		}

		return mmbackend_strdup(&cfg.mdns_name, value);
	}
	else if(!strcmp(option, "mdns-interface")){
		if(cfg.mdns_interface){
			LOG("Duplicate mdns-interface assignment");
			return 1;
		}

		return mmbackend_strdup(&cfg.mdns_interface, value);
	}
	else if(!strcmp(option, "detect")){
		cfg.detect = 0;
		if(!strcmp(value, "on")){
			cfg.detect = 1;
		}
		return 0;
	}

	LOGPF("Unknown backend configuration option %s", option);
	return 1;
}

static int rtpmidi_bind_instance(instance* inst, rtpmidi_instance_data* data, char* host, char* port){
	struct sockaddr_storage sock_addr = {
		0
	};
	socklen_t sock_len = sizeof(sock_addr);
	char control_port[32];

	//bind to random port if none supplied
	data->fd = mmbackend_socket(host, port ? port : "0", SOCK_DGRAM, 1, 0, 1);
	if(data->fd < 0){
		return 1;
	}

	if(getsockname(data->fd, (struct sockaddr*) &sock_addr, &sock_len)){
		LOGPF("Failed to fetch data port information: %s", mmbackend_socket_strerror(errno));
		return 1;
	}

	//bind control port
	if(data->mode == apple){
		data->control_port = be16toh(((struct sockaddr_in*) &sock_addr)->sin_port) - 1;
		snprintf(control_port, sizeof(control_port), "%d", data->control_port);
		data->control_fd = mmbackend_socket(host, control_port, SOCK_DGRAM, 1, 0, 1);
		if(data->control_fd < 0){
			LOGPF("Failed to bind control port %s for instance %s", control_port, inst->name);
			return 1;
		}

		LOGPF("Apple mode instance %s listening on ports %d (control) and %d (data)", inst->name, data->control_port, data->control_port + 1);
	}
	else{
		data->control_port = be16toh(((struct sockaddr_in*)&sock_addr)->sin_port);
		LOGPF("Direct mode instance %s listening on port %d", inst->name, data->control_port);
	}

	return 0;
}

static char* rtpmidi_type_name(uint8_t type){
	switch(type){
		case note:
			return "note";
		case note_off:
			return "note_off";
		case cc:
			return "cc";
		case pressure:
			return "pressure";
		case aftertouch:
			return "aftertouch";
		case pitchbend:
			return "pitch";
		case program:
			return "program";
		case rpn:
			return "rpn";
		case nrpn:
			return "nrpn";
	}
	return "unknown";
}

static int rtpmidi_push_peer(rtpmidi_instance_data* data, struct sockaddr* sock_addr, socklen_t sock_len, uint8_t learned, uint8_t connected, ssize_t invite_reference){
	size_t u, p = data->peers;

	for(u = 0; u < data->peers; u++){
		//check whether the peer is already in the list
		//TODO this probably should take into account the invite_reference (-1 for initiator peers or if unknown but may be present)
		if(data->peer[u].active
				&& sock_len == data->peer[u].dest_len
				&& !memcmp(&data->peer[u].dest, sock_addr, sock_len)){
			//if yes, update connection flag (but not learned flag because that doesn't change)
			data->peer[u].connected = connected;
			return 0;
		}

		if(!data->peer[u].active){
			p = u;
		}
	}

	if(p == data->peers){
		data->peer = realloc(data->peer, (data->peers + 1) * sizeof(rtpmidi_peer));
		if(!data->peer){
			LOG("Failed to allocate memory");
			data->peers = 0;
			return 1;
		}
		data->peers++;
		DBGPF("Extending peer registry to %" PRIsize_t " entries", data->peers);
	}

	data->peer[p].active = 1;
	data->peer[p].learned = learned;
	data->peer[p].connected = connected;
	data->peer[p].invite = invite_reference;
	memcpy(&(data->peer[p].dest), sock_addr, sock_len);
	data->peer[p].dest_len = sock_len;
	return 0;
}

static int rtpmidi_push_invite(instance* inst, char* peer){
	size_t u, p;

	//check whether the instance is already in the inviter list
	for(u = 0; u < cfg.invites; u++){
		if(cfg.invite[u].inst == inst){
			break;
		}
	}

	//add to the inviter list
	if(u == cfg.invites){
		cfg.invite = realloc(cfg.invite, (cfg.invites + 1) * sizeof(rtpmidi_invite));
		if(!cfg.invite){
			LOG("Failed to allocate memory");
			cfg.invites = 0;
			return 1;
		}

		cfg.invite[u].inst = inst;
		cfg.invite[u].invites = 0;
		cfg.invite[u].name = NULL;

		cfg.invites++;
	}

	//check whether the requested name is already in the invite list for this instance
	for(p = 0; p < cfg.invite[u].invites; p++){
		if(!strcmp(cfg.invite[u].name[p], peer)){
			return 0;
		}
	}

	//extend the invite list
	cfg.invite[u].name = realloc(cfg.invite[u].name, (cfg.invite[u].invites + 1) * sizeof(char*));
	if(!cfg.invite[u].name){
		LOG("Failed to allocate memory");
		cfg.invite[u].invites = 0;
		return 1;
	}

	//append the new invitee
	cfg.invite[u].name[p] = strdup(peer);
	if(!cfg.invite[u].name[p]){
		LOG("Failed to allocate memory");
		return 1;
	}

	cfg.invite[u].invites++;
	return 0;
}

static int rtpmidi_applecommand(instance* inst, struct sockaddr* dest, socklen_t dest_len, uint8_t control, applemidi_command command, uint32_t token){
	rtpmidi_instance_data* data = (rtpmidi_instance_data*) inst->impl;
	uint8_t frame[RTPMIDI_PACKET_BUFFER] = "";
	ssize_t bytes = 0;

	apple_command* cmd = (apple_command*) &frame;
	cmd->res1 = 0xFFFF;
	cmd->command = htobe16(command);
	cmd->version = htobe32(2);
	cmd->token = token ? token : (((uint32_t) rand()) << 16 | rand());
	cmd->ssrc = htobe32(data->ssrc);

	//append session name to packet
	memcpy(frame + sizeof(apple_command), inst->name, strlen(inst->name) + 1);

	//FIXME should we match sending/receiving ports? if the reference does this, it should be documented
	bytes = sendto(control ? data->control_fd : data->fd, frame, sizeof(apple_command) + strlen(inst->name) + 1, 0, dest, dest_len);
	if(bytes != sizeof(apple_command) + strlen(inst->name) + 1){
		LOGPF("Failed to transmit session command on %s: %s", inst->name, mmbackend_socket_strerror(errno));
		return 1;
	}
	return 0;
}

static int rtpmidi_peer_applecommand(instance* inst, size_t peer, uint8_t control, applemidi_command command, uint32_t token){
	rtpmidi_instance_data* data = (rtpmidi_instance_data*) inst->impl;
	struct sockaddr_storage dest_addr;

	memcpy(&dest_addr, &(data->peer[peer].dest), min(sizeof(dest_addr), data->peer[peer].dest_len));
	if(control){
		//calculate remote control port from data port
		((struct sockaddr_in*) &dest_addr)->sin_port = htobe16(be16toh(((struct sockaddr_in*) &dest_addr)->sin_port) - 1);
	}

	return rtpmidi_applecommand(inst, (struct sockaddr*) &dest_addr, data->peer[peer].dest_len, control, command, token);
}

static int rtpmidi_configure_instance(instance* inst, char* option, char* value){
	rtpmidi_instance_data* data = (rtpmidi_instance_data*) inst->impl;
	char* host = NULL, *port = NULL;
	struct sockaddr_storage sock_addr;
	socklen_t sock_len = sizeof(sock_addr);

	if(!strcmp(option, "mode")){
		if(!strcmp(value, "direct")){
			data->mode = direct;
			return 0;
		}
		else if(!strcmp(value, "apple")){
			data->mode = apple;
			return 0;
		}
		LOGPF("Unknown instance mode %s for instance %s", value, inst->name);
		return 1;
	}
	else if(!strcmp(option, "epn-tx")){
		data->epn_tx_short = 0;
		if(!strcmp(value, "short")){
			data->epn_tx_short = 1;
		}
		return 0;
	}
	else if(!strcmp(option, "ssrc")){
		data->ssrc = strtoul(value, NULL, 0);
		if(!data->ssrc){
			LOGPF("Random SSRC will be generated for instance %s", inst->name);
		}
		return 0;
	}
	else if(!strcmp(option, "bind")){
		if(data->mode == unconfigured){
			LOGPF("Please specify mode for instance %s before setting bind host", inst->name);
			return 1;
		}

		mmbackend_parse_hostspec(value, &host, &port, NULL);

		if(!host){
			LOGPF("Could not parse bind host specification %s for instance %s", value, inst->name);
			return 1;
		}

		return rtpmidi_bind_instance(inst, data, host, port);
	}
	else if(!strcmp(option, "learn")){
		if(data->mode != direct){
			LOG("'learn' option is only valid for direct mode instances");
			return 1;
		}
		data->learn_peers = 0;
		if(!strcmp(value, "true")){
			data->learn_peers = 1;
		}
		return 0;
	}
	else if(!strcmp(option, "peer")){
		if(data->mode == unconfigured){
			LOGPF("Please specify mode for instance %s before configuring peers", inst->name);
			return 1;
		}

		mmbackend_parse_hostspec(value, &host, &port, NULL);
		if(!host || !port){
			LOGPF("Invalid peer %s configured on instance %s", value, inst->name);
			return 1;
		}

		if(mmbackend_parse_sockaddr(host, port, &sock_addr, &sock_len)){
			LOGPF("Failed to resolve peer %s on instance %s", value, inst->name);
			return 1;
		}

		//apple peers are specified using the control port, but we want to store the data port as peer
		if(data->mode == apple){
			((struct sockaddr_in*) &sock_addr)->sin_port = htobe16(be16toh(((struct sockaddr_in*) &sock_addr)->sin_port) + 1);
		}

		return rtpmidi_push_peer(data, (struct sockaddr*) &sock_addr, sock_len, 0, 0, -1);
	}
	else if(!strcmp(option, "invite")){
		if(data->mode != apple){
			LOG("'invite' option is only valid for apple mode instances");
			return 1;
		}

		return rtpmidi_push_invite(inst, value);
	}
	else if(!strcmp(option, "join")){
		if(data->mode != apple){
			LOG("'join' option is only valid for apple mode instances");
			return 1;
		}
		return mmbackend_strdup(&data->accept, value);
	}
	else if(!strcmp(option, "note-off")){
		data->note_off = 0;
		if(!strcmp(value, "true")){
			data->note_off = 1;
		}
		return 0;
	}

	LOGPF("Unknown instance configuration option %s on instance %s", option, inst->name);
	return 1;
}

static int rtpmidi_instance(instance* inst){
	rtpmidi_instance_data* data = calloc(1, sizeof(rtpmidi_instance_data));
	if(!data){
		LOG("Failed to allocate memory");
		return 1;
	}
	data->fd = -1;
	data->control_fd = -1;

	inst->impl = data;
	return 0;
}

static channel* rtpmidi_channel(instance* inst, char* spec, uint8_t flags){
	char* next_token = spec;
	rtpmidi_channel_ident ident = {
		.label = 0
	};

	if(!strncmp(spec, "ch", 2)){
		next_token += 2;
		if(!strncmp(spec, "channel", 7)){
			next_token = spec + 7;
		}
	}
	else{
		LOGPF("Invalid channel specification %s", spec);
		return NULL;
	}

	ident.fields.channel = strtoul(next_token, &next_token, 10);
	if(ident.fields.channel > 15){
		LOGPF("Channel out of range in channel spec %s", spec);
		return NULL;
	}

	if(*next_token != '.'){
		LOGPF("Channel specification %s does not conform to channel<X>.<control><Y>", spec);
		return NULL;
	}

	next_token++;

	if(!strncmp(next_token, "cc", 2)){
		ident.fields.type = cc;
		next_token += 2;
	}
	else if(!strncmp(next_token, "note_off", 8)){	// note_off needs to be checked first because of the way that strncomp is being used here
		ident.fields.type = note_off;
		next_token += 8;
	}
	else if(!strncmp(next_token, "note", 4)){
		ident.fields.type = note;
		next_token += 4;
	}
	else if(!strncmp(next_token, "rpn", 3)){
		ident.fields.type = rpn;
		next_token += 3;
	}
	else if(!strncmp(next_token, "nrpn", 4)){
		ident.fields.type = nrpn;
		next_token += 4;
	}
	else if(!strncmp(next_token, "pressure", 8)){
		ident.fields.type = pressure;
		next_token += 8;
	}
	else if(!strncmp(next_token, "pitch", 5)){
		ident.fields.type = pitchbend;
	}
	else if(!strncmp(next_token, "aftertouch", 10)){
		ident.fields.type = aftertouch;
	}
	else if(!strncmp(next_token, "program", 7)){
		ident.fields.type = program;
	}
	else{
		LOGPF("Unknown control type in spec %s", spec);
		return NULL;
	}

	ident.fields.control = strtoul(next_token, NULL, 10);

	if(ident.label){
		return mm_channel(inst, ident.label, 1);
	}
	return NULL;
}

static size_t rtpmidi_push_midi(uint8_t* payload, size_t bytes_left, uint8_t type, uint8_t channel, uint8_t control, uint16_t value){
	//FIXME this is a bit simplistic but it works for now
	if(bytes_left < 4){
		return 0;
	}

	//encode timestamp
	payload[0] = 0;

	//encode midi command
	payload[1] = type | channel;
	payload[2] = control;
	payload[3] = value & 0x7F;

	if(type == pitchbend){
		payload[2] = value & 0x7F;
		payload[3] = (value >> 7) & 0x7F;
	}
	//channel-wides aftertouch and program are only 2 bytes
	else if(type == aftertouch || type == program){
		payload[2] = payload[3];
		return 3;
	}
	return 4;
}

static int rtpmidi_set(instance* inst, size_t num, channel** c, channel_value* v){
	rtpmidi_instance_data* data = (rtpmidi_instance_data*) inst->impl;
	uint8_t frame[RTPMIDI_PACKET_BUFFER] = {0};
	rtpmidi_header* rtp_header = (rtpmidi_header*) frame;
	rtpmidi_command_header* command_header = (rtpmidi_command_header*) (frame + sizeof(rtpmidi_header));
	size_t command_length = 0, offset = sizeof(rtpmidi_header) + sizeof(rtpmidi_command_header), u = 0;
	uint8_t* payload = frame;
	rtpmidi_channel_ident ident;

	rtp_header->vpxcc = RTPMIDI_HEADER_MAGIC;
	//some receivers seem to have problems reading rfcs and interpreting the marker bit correctly
	rtp_header->mpt = (data->mode == apple ? 0 : 0x80) | RTPMIDI_HEADER_TYPE;
	rtp_header->sequence = htobe16(data->sequence++); // big-endian
	rtp_header->timestamp = htobe32(mm_timestamp() * 10); //big-endian - just assume 100msec resolution because rfc4695 handwaves it
	rtp_header->ssrc = htobe32(data->ssrc); //big-endian

	//midi command section header
	//TODO enable the journal bit here
	command_header->flags = 0xA0; //extended length header, first entry in list has dtime

	// command_header->flags = 0x80; 

	//midi list
	for(u = 0; u < num; u++){
		ident.label = c[u]->ident;

		switch(ident.fields.type){
			case rpn:
			case nrpn:
				//transmit parameter number
				command_length = rtpmidi_push_midi(payload + offset, sizeof(frame) - offset, cc, ident.fields.channel, (ident.fields.type == rpn) ? 101 : 99, (ident.fields.control >> 7) & 0x7F);
				command_length += rtpmidi_push_midi(payload + offset + command_length, sizeof(frame) - offset, cc, ident.fields.channel, (ident.fields.type == rpn) ? 100 : 98, ident.fields.control & 0x7F);

				//transmit parameter value
				command_length += rtpmidi_push_midi(payload + offset + command_length, sizeof(frame) - offset, cc, ident.fields.channel, 6, (((uint16_t) (v[u].normalised * 16383.0)) >> 7) & 0x7F);
				command_length += rtpmidi_push_midi(payload + offset + command_length, sizeof(frame) - offset, cc, ident.fields.channel, 38, ((uint16_t) (v[u].normalised * 16383.0)) & 0x7F);

				if(!data->epn_tx_short){
					//clear active parameter
					command_length += rtpmidi_push_midi(payload + offset + command_length, sizeof(frame) - offset, cc, ident.fields.channel, 101, 127);
					command_length += rtpmidi_push_midi(payload + offset + command_length, sizeof(frame) - offset, cc, ident.fields.channel, 100, 127);
				}
				break;
			case pitchbend:
				//TODO check whether this works
				command_length = rtpmidi_push_midi(payload + offset, sizeof(frame) - offset, ident.fields.type, ident.fields.channel, ident.fields.control, v[u].normalised * 16383.0);
				break;
			default:
				command_length = rtpmidi_push_midi(payload + offset, sizeof(frame) - offset, ident.fields.type, ident.fields.channel, ident.fields.control, v[u].normalised * 127.0);
		}

		if(command_length == 0){
			LOGPF("Transmit buffer size exceeded on %s", inst->name);
			break;
		}

		offset += command_length;
	}

	//update command section length
	//FIXME this might overrun, might check the number of events at some point
	command_header->flags |= (((offset - sizeof(rtpmidi_header) - sizeof(rtpmidi_command_header)) & 0x0F00) >> 8);
	command_header->length = ((offset - sizeof(rtpmidi_header) - sizeof(rtpmidi_command_header)) & 0xFF);

	//TODO journal section

	for(u = 0; u < data->peers; u++){
		if(data->peer[u].active && data->peer[u].connected){
			if(sendto(data->fd, frame, offset, 0, (struct sockaddr*) &data->peer[u].dest, data->peer[u].dest_len) <= 0){
				LOGPF("Failed to transmit to peer: %s", mmbackend_socket_strerror(errno));
			}
		}
	}

	return 0;
}

static int rtpmidi_handle_applemidi(instance* inst, int fd, uint8_t* frame, size_t bytes, struct sockaddr_storage* peer, socklen_t peer_len){
	rtpmidi_instance_data* data = (rtpmidi_instance_data*) inst->impl;
	uint8_t response[RTPMIDI_PACKET_BUFFER] = "";
	apple_command* command = (apple_command*) frame;
	char* session_name = (char*) frame + sizeof(apple_command);
	size_t n, u;

	command->command = be16toh(command->command);

	//check command version (except for clock sync and receiver feedback)
	if(command->command != apple_sync && command->command != apple_feedback
			&& be32toh(command->version) != 2){
		LOGPF("Invalid AppleMIDI command version %" PRIu32 " on instance %s", be32toh(command->version), inst->name);
		return 0;
	}

	if(command->command == apple_invite){
		//check session name
		for(n = sizeof(apple_command); n < bytes; n++){
			if(!frame[n]){
				break;
			}

			if(!isprint(frame[n])){
				session_name = NULL;
				break;
			}
		}

		//unterminated string
		if(n == bytes){
			session_name = NULL;
		}

		//FIXME if already in session, reject the invitation
		if(data->accept &&
				(!strcmp(data->accept, "*") || (session_name && !strcmp(session_name, data->accept)))){
			//accept the invitation
			LOGPF("Instance %s accepting invitation to session %s%s", inst->name, session_name ? session_name : "UNNAMED", (fd == data->control_fd) ? " (control)":"");
			//send accept message
			rtpmidi_applecommand(inst, (struct sockaddr*) peer, peer_len, (fd == data->control_fd) ? 1 : 0, apple_accept, command->token);

			//push peer
			if(fd != data->control_fd){
				return rtpmidi_push_peer(data, (struct sockaddr*) peer, peer_len, 1, 1, -1);
			}
			return 0;
		}
		else{
			//send reject message
			LOGPF("Instance %s rejecting invitation to session %s", inst->name, session_name ? session_name : "UNNAMED");
			rtpmidi_applecommand(inst, (struct sockaddr*) peer, peer_len, (fd == data->control_fd) ? 1 : 0, apple_reject, command->token);
		}
		return 0;
	}
	else if(command->command == apple_accept){
		if(fd != data->control_fd){
			LOGPF("Instance %s negotiated new peer", inst->name);
			return rtpmidi_push_peer(data, (struct sockaddr*) peer, peer_len, 1, 1, -1);
			//FIXME store ssrc, start timesync
		}
		else{
			//invite peer data port
			LOGPF("Instance %s peer accepted on control port, inviting data port", inst->name);
			//calculate data port
			((struct sockaddr_in*) peer)->sin_port = htobe16(be16toh(((struct sockaddr_in*) peer)->sin_port) + 1);
			//send invite
			rtpmidi_applecommand(inst, (struct sockaddr*) peer, peer_len, 0, apple_invite, 0);
		}
		return 0;
	}
	else if(command->command == apple_reject){
		//just ignore this for now and retry the invitation
		LOGPF("Invitation rejected on instance %s", inst->name);
	}
	else if(command->command == apple_leave){
		//remove peer from list - this comes in on the control port, but we need to remove the data port...
		((struct sockaddr_in*) peer)->sin_port = htobe16(be16toh(((struct sockaddr_in*) peer)->sin_port) + 1);
		for(u = 0; u < data->peers; u++){
			if(data->peer[u].dest_len == peer_len
					&& !memcmp(&data->peer[u].dest, peer, peer_len)){
				LOGPF("Instance %s removed peer", inst->name);
				//learned peers are marked inactive, configured peers are marked unconnected
				if(data->peer[u].learned){
					data->peer[u].active = 0;
				}
				else{
					data->peer[u].connected = 0;
				}
			}
		}
		return 0;
	}
	else if(command->command == apple_sync){
		//respond with sync answer
		memcpy(response, frame, bytes);
		apple_sync_frame* sync = (apple_sync_frame*) response;
		DBGPF("Incoming sync on instance %s (%d)", inst->name, sync->count);
		sync->command = htobe16(apple_sync);
		sync->ssrc = htobe32(data->ssrc);
		switch(sync->count){
			case 0:
				//this happens if we're a participant
				sync->count++;
				sync->timestamp[1] = htobe64(mm_timestamp() * 10);
				break;
			case 1:
				//this happens if we're an initiator
				sync->count++;
				sync->timestamp[2] = htobe64(mm_timestamp() * 10);
				break;
			default:
				//ignore this one
				return 0;
		}

		if(sendto(fd, response, sizeof(apple_sync_frame), 0, (struct sockaddr*) peer, peer_len) != sizeof(apple_sync_frame)){
			LOG("Failed to output sync frame");
		}
		return 0;
	}
	else if(command->command == apple_feedback){
		//TODO store this somewhere to properly update the recovery journal
		LOGPF("Feedback on instance %s", inst->name);
		return 0;
	}
	else{
		LOGPF("Unknown AppleMIDI session command %04X", command->command);
	}

	return 0;
}

//this state machine was copied more-or-less verbatim from the alsa midi implementation - fixes there will need to be integrated
static void rtpmidi_handle_epn(instance* inst, uint8_t chan, uint16_t control, uint16_t value){
	rtpmidi_instance_data* data = (rtpmidi_instance_data*) inst->impl;
	rtpmidi_channel_ident ident = {
		.label = 0
	};
	channel* changed = NULL;
	channel_value val;

	//switching between nrpn and rpn clears all valid bits
	if(((data->epn_status[chan] & EPN_NRPN) && (control == 101 || control == 100))
				|| (!(data->epn_status[chan] & EPN_NRPN) && (control == 99 || control == 98))){
		data->epn_status[chan] &= ~(EPN_NRPN | EPN_PARAMETER_LO | EPN_PARAMETER_HI);
	}

	//setting an address always invalidates the value valid bits
	if(control >= 98 && control <= 101){
		data->epn_status[chan] &= ~EPN_VALUE_HI;
	}

	//parameter hi
	if(control == 101 || control == 99){
		data->epn_control[chan] &= 0x7F;
		data->epn_control[chan] |= value << 7;
		data->epn_status[chan] |= EPN_PARAMETER_HI | ((control == 99) ? EPN_NRPN : 0);
		if(control == 101 && value == 127){
			data->epn_status[chan] &= ~EPN_PARAMETER_HI;
		}
	}

	//parameter lo
	if(control == 100 || control == 98){
		data->epn_control[chan] &= ~0x7F;
		data->epn_control[chan] |= value & 0x7F;
		data->epn_status[chan] |= EPN_PARAMETER_LO | ((control == 98) ? EPN_NRPN : 0);
		if(control == 100 && value == 127){
			data->epn_status[chan] &= ~EPN_PARAMETER_LO;
		}
	}

	//value hi, clears low, mark as update candidate
	if(control == 6
			//check if parameter is set before accepting value update
			&& ((data->epn_status[chan] & (EPN_PARAMETER_HI | EPN_PARAMETER_LO)) == (EPN_PARAMETER_HI | EPN_PARAMETER_LO))){
		data->epn_value[chan] = value << 7;
		data->epn_status[chan] |= EPN_VALUE_HI;
	}

	//value lo, flush the value
	if(control == 38
			&& data->epn_status[chan] & EPN_VALUE_HI){
		data->epn_value[chan] &= ~0x7F;
		data->epn_value[chan] |= value & 0x7F;
		data->epn_status[chan] &= ~EPN_VALUE_HI;

		if(cfg.detect){
			LOGPF("Incoming EPN data on channel %s.ch%d.%s%d", inst->name, chan, data->epn_status[chan] & EPN_NRPN ? "nrpn" : "rpn", data->epn_control[chan]);
		}

		//find the updated channel
		ident.fields.type = data->epn_status[chan] & EPN_NRPN ? nrpn : rpn;
		ident.fields.channel = chan;
		ident.fields.control = data->epn_control[chan];
		val.normalised = (double) data->epn_value[chan] / 16383.0;

		//push the new value
		changed = mm_channel(inst, ident.label, 0);
		if(changed){
			mm_channel_event(changed, val);
		}
	}
}

static int rtpmidi_parse(instance* inst, uint8_t* frame, size_t bytes){
	rtpmidi_instance_data* data = (rtpmidi_instance_data*) inst->impl;
	uint16_t length = 0;
	size_t offset = 1, decode_time = 0, command_bytes = 0;
	uint8_t midi_status = 0;
	rtpmidi_channel_ident ident;
	channel_value val;
	channel* chan = NULL;

	if(!bytes){
		LOGPF("No command section in data on instance %s", inst->name);
		return 1;
	}

	//calculate midi command section length
	length = frame[0] & 0x0F;
	if(frame[0] & 0x80){
		//extended header
		if(bytes < 2){
			LOGPF("Short command section (%" PRIsize_t " bytes) on %s, missing extended header", bytes, inst->name);
			return 1;
		}
		length <<= 8;
		length |= frame[1];
		offset = 2;
	}

	command_bytes = offset + length;
	DBGPF("%u/%" PRIsize_t " bytes of command section on %s, %s header, %s initial dtime",
			length, bytes, inst->name,
			(frame[0] & 0x80) ? "extended" : "normal",
			(frame[0] & 0x20) ? "has" : "no");

	if(command_bytes > bytes){
		LOGPF("Short command section on %s, indicated %" PRIsize_t ", had %" PRIsize_t, inst->name, command_bytes, bytes);
		return 1;
	}

	if(frame[0] & 0x20){
		decode_time = 1;
	}

	do{
		//decode (and ignore) delta-time
		if(decode_time){
			for(; offset < command_bytes && frame[offset] & 0x80; offset++){
			}
			offset++;
		}

		//section 3 of rfc6295 states that the first dtime as well as the last command may be omitted
		//this may make sense on a low-speed serial line, but on a network... come on.
		if(offset >= command_bytes){
			break;
		}

		//check for a status byte
		//TODO filter sysex
		if(frame[offset] & 0x80){
			midi_status = frame[offset];
			offset++;
		}

		//having variable encoding in each and every component is super annoying to check for...
		if(offset >= command_bytes){
			break;
		}

		ident.label = 0;
		ident.fields.type = midi_status & 0xF0;
		ident.fields.channel = midi_status & 0x0F;

		//single byte command
		if(ident.fields.type == aftertouch || ident.fields.type == program){
			ident.fields.control = 0;
			val.normalised = (double) frame[offset] / 127.0;
			val.raw.u64 = frame[offset];
			offset++;
		}
		//two-byte command
		else{
			offset++;
			if(offset >= command_bytes){
				break;
			}

			if(ident.fields.type == pitchbend){
				ident.fields.control = 0;
				val.normalised = (double)((frame[offset] << 7) | frame[offset - 1]) / 16383.0;
				val.raw.u64 = (frame[offset] << 7) | frame[offset - 1];
			}
			else{
				ident.fields.control = frame[offset - 1];
				val.normalised = (double) frame[offset] / 127.0;
				val.raw.u64 = frame[offset];
			}

			//fix-up note off events
			if (data->note_off == 0 && ident.fields.type == note_off) {
				ident.fields.type = note;
				val.normalised = 0;
				val.raw.u64 = 0;
			}

			offset++;
		}

		DBGPF("Decoded command type %02X channel %d control %d value %f",
				ident.fields.type, ident.fields.channel, ident.fields.control, val.normalised);

		//forward EPN CCs to the EPN state machine
		if(ident.fields.type == cc
				&& ((ident.fields.control <= 101 && ident.fields.control >= 98)
					|| ident.fields.control == 6
					|| ident.fields.control == 38)){
			rtpmidi_handle_epn(inst, ident.fields.channel, ident.fields.control, val.raw.u64);
		}	

		if(cfg.detect){
			if(ident.fields.type == pitchbend
					|| ident.fields.type == aftertouch
					|| ident.fields.type == program){
				LOGPF("Incoming data on channel %s.ch%d.%s, value %f",
						inst->name, ident.fields.channel,
						rtpmidi_type_name(ident.fields.type), val.normalised);
			}
			else{
				LOGPF("Incoming data on channel %s.ch%d.%s%d, value %f",
						inst->name, ident.fields.channel,
						rtpmidi_type_name(ident.fields.type),
						ident.fields.control, val.normalised);
			}
		}

		//push event
		chan = mm_channel(inst, ident.label, 0);
		if(chan){
			mm_channel_event(chan, val);
		}

		decode_time = 1;
	} while(offset < command_bytes);

	return 0;
}

static int rtpmidi_handle_data(instance* inst){
	rtpmidi_instance_data* data = (rtpmidi_instance_data*) inst->impl;
	uint8_t frame[RTPMIDI_PACKET_BUFFER] = "";
	struct sockaddr_storage sock_addr;
	socklen_t sock_len = sizeof(sock_addr);
	rtpmidi_header* rtp_header = (rtpmidi_header*) frame;
	ssize_t bytes_recv = recvfrom(data->fd, frame, sizeof(frame), 0, (struct sockaddr*) &sock_addr, &sock_len);
	size_t u;

	//TODO receive until EAGAIN
	if(bytes_recv < 0){
		LOGPF("Failed to receive for instance %s", inst->name);
		return 1;
	}

	if(bytes_recv < sizeof(rtpmidi_header)){
		LOGPF("Skipping short packet on instance %s", inst->name);
		return 0;
	}

	//FIXME might want to filter data input from sources that are not registered peers
	if(data->mode == apple && rtp_header->vpxcc == 0xFF && rtp_header->mpt == 0xFF){
		return rtpmidi_handle_applemidi(inst, data->fd, frame, bytes_recv, &sock_addr, sock_len);
	}
	else if(rtp_header->vpxcc != RTPMIDI_HEADER_MAGIC || RTPMIDI_GET_TYPE(rtp_header->mpt) != RTPMIDI_HEADER_TYPE){
		LOGPF("Frame with invalid header magic on %s", inst->name);
		return 0;
	}

	//parse data
	if(rtpmidi_parse(inst, frame + sizeof(rtpmidi_header), bytes_recv - sizeof(rtpmidi_header))){
		//returning errors here fails the core loop, so just return 0 to have some logging
		return 0;
	}

	//try to learn peers
	if(data->learn_peers){
		for(u = 0; u < data->peers; u++){
			if(data->peer[u].active
					&& data->peer[u].dest_len == sock_len
					&& !memcmp(&data->peer[u].dest, &sock_addr, sock_len)){
				break;
			}
		}

		if(u == data->peers){
			LOGPF("Learned new peer on %s", inst->name);
			return rtpmidi_push_peer(data, (struct sockaddr*) &sock_addr, sock_len, 1, 1, -1);
		}
	}
	return 0;
}

static int rtpmidi_handle_control(instance* inst){
	rtpmidi_instance_data* data = (rtpmidi_instance_data*) inst->impl;
	uint8_t frame[RTPMIDI_PACKET_BUFFER] = "";
	struct sockaddr_storage sock_addr;
	socklen_t sock_len = sizeof(sock_addr);
	ssize_t bytes_recv = recvfrom(data->control_fd, frame, sizeof(frame), 0, (struct sockaddr*) &sock_addr, &sock_len);

	if(bytes_recv < 0){
		LOGPF("Failed to receive on control socket for instance %s", inst->name);
		return 1;
	}

	//the shortest applemidi packet is still larger than the rtpmidi header, so use that as bar
	if(bytes_recv < sizeof(rtpmidi_header)){
		LOGPF("Skipping short packet on control socket of instance %s", inst->name);
		return 0;
	}

	if(data->mode == apple && frame[0] == 0xFF && frame[1] == 0xFF){
		return rtpmidi_handle_applemidi(inst, data->control_fd, frame, bytes_recv, &sock_addr, sock_len);
	}

	LOGPF("Unknown session protocol frame received on instance %s", inst->name);
	return 0;
}

static int rtpmidi_mdns_broadcast(uint8_t* frame, size_t len){
	struct sockaddr_in mcast = {
		.sin_family = AF_INET,
		.sin_port = htobe16(5353),
		.sin_addr.s_addr = htobe32(((uint32_t) 0xe00000fb))
	};
	struct sockaddr_in6 mcast6 = {
		.sin6_family = AF_INET6,
		.sin6_port = htobe16(5353),
		.sin6_addr.s6_addr = {0xff, 0x02, 0x00, 0x00,
				0x00, 0x00, 0x00, 0x00,
				0x00, 0x00, 0x00, 0x00,
				0x00, 0x00, 0x00, 0xfb}
	};

	//send to ipv4 and ipv6 mcasts
	if((sendto(cfg.mdns_fd, frame, len, 0, (struct sockaddr*) &mcast6, sizeof(mcast6)) != len)
			| (sendto(cfg.mdns4_fd, frame, len, 0, (struct sockaddr*) &mcast, sizeof(mcast)) != len)){
		LOG("Failed to transmit mDNS frame");
	}
	return 0;
}

static int rtpmidi_mdns_detach(instance* inst){
	uint8_t frame[RTPMIDI_PACKET_BUFFER] = "";
	dns_header* hdr = (dns_header*) frame;
	dns_rr* rr = NULL;
	dns_name name = {
		.alloc = 0
	};
	size_t offset = 0;
	ssize_t bytes = 0;

	hdr->id = 0;
	hdr->flags[0] = 0x84;
	hdr->flags[1] = 0;
	hdr->questions = hdr->servers = hdr->additional = 0;
	hdr->answers = htobe16(1);
	offset = sizeof(dns_header);

	//answer 1: _apple-midi PTR FQDN
	snprintf((char*) frame + offset, sizeof(frame) - offset, "%s", RTPMIDI_MDNS_DOMAIN);
	bytes = dns_push_rr(frame + offset, sizeof(frame) - offset, &rr, (char*) frame + offset, 12, 1, 0, 0);
	if(bytes < 0){
		goto bail;
	}
	offset += bytes;

	//calculate maximum permitted instance name length
	bytes = min(min(strlen(inst->name), sizeof(frame) - offset - 3), 255);
	frame[offset++] = bytes;
	memcpy(frame + offset, inst->name, bytes);
	offset += bytes;
	frame[offset++] = 0xC0;
	frame[offset++] = sizeof(dns_header);
	rr->data = htobe16(1 + bytes + 2);

	free(name.name);
	return rtpmidi_mdns_broadcast(frame, offset);
bail:
	free(name.name);
	return 1;
}

//FIXME this should not exceed 1500 bytes
static int rtpmidi_mdns_announce(instance* inst){
	rtpmidi_instance_data* data = (rtpmidi_instance_data*) inst->impl;
	uint8_t frame[RTPMIDI_PACKET_BUFFER] = "";
	dns_header* hdr = (dns_header*) frame;
	dns_rr* rr = NULL;
	dns_rr_srv* srv = NULL;
	dns_name name = {
		.alloc = 0
	};
	size_t offset = 0, host_offset = 0, u = 0;
	ssize_t bytes = 0;

	hdr->id = 0;
	hdr->flags[0] = 0x84;
	hdr->flags[1] = 0;
	hdr->questions = hdr->servers = 0;
	hdr->answers = htobe16(4);
	hdr->additional = htobe16(cfg.addresses);
	offset = sizeof(dns_header);

	//answer 1: SRV FQDN
	snprintf((char*) frame + offset, sizeof(frame) - offset, "%s.%s", inst->name, RTPMIDI_MDNS_DOMAIN);
	bytes = dns_push_rr(frame + offset, sizeof(frame) - offset, &rr, (char*) frame + offset, 33, 1, 120, 0);
	if(bytes < 0){
		goto bail;
	}
	offset += bytes;

	srv = (dns_rr_srv*) (frame + offset);
	srv->priority = 0;
	srv->weight = 0;
	srv->port = htobe16(data->control_port);
	offset += sizeof(dns_rr_srv);

	//rfc2782 (srv) says to not compress `target`, rfc6762 (mdns) 18.14 says to
	//we don't do it because i don't want to
	snprintf((char*) frame + offset, sizeof(frame) - offset, "%s.local", cfg.mdns_name);
	if(dns_encode_name((char*) frame + offset, &name)){
		LOGPF("Failed to encode name for %s", frame + offset);
		goto bail;
	}
	memcpy(frame + offset, name.name, name.length);
	offset += name.length;
	rr->data = htobe16(sizeof(dns_rr_srv) + name.length);

	//answer 2: empty TXT (apple asks for it otherwise)
	frame[offset++] = 0xC0;
	frame[offset++] = sizeof(dns_header);

	bytes = dns_push_rr(frame + offset, sizeof(frame) - offset, &rr, NULL, 16, 1, 4500, 1);
	if(bytes < 0){
		goto bail;
	}
	offset += bytes;
	frame[offset++] = 0x00; //zero-length TXT

	//answer 3: dns-sd PTR _applemidi
	snprintf((char*) frame + offset, sizeof(frame) - offset, "%s", RTPMIDI_DNSSD_DOMAIN);
	bytes = dns_push_rr(frame + offset, sizeof(frame) - offset, &rr, (char*) frame + offset, 12, 1, 4500, 2);
	if(bytes < 0){
		goto bail;
	}
	offset += bytes;

	//add backref for PTR
	frame[offset++] = 0xC0;
	frame[offset++] = sizeof(dns_header) + frame[sizeof(dns_header)] + 1;

	//answer 4: _applemidi PTR FQDN
	frame[offset++] = 0xC0;
	frame[offset++] = sizeof(dns_header) + frame[sizeof(dns_header)] + 1;

	bytes = dns_push_rr(frame + offset, sizeof(frame) - offset, &rr, NULL, 12, 1, 4500, 2);
	if(bytes < 0){
		goto bail;
	}
	offset += bytes;

	//add backref for PTR
	frame[offset++] = 0xC0;
	frame[offset++] = sizeof(dns_header);

	//additional 1: first announce addr
	host_offset = offset;
	snprintf((char*) frame + offset, sizeof(frame) - offset, "%s.local", cfg.mdns_name);
	bytes = dns_push_rr(frame + offset, sizeof(frame) - offset, &rr, (char*) frame + offset,
			(cfg.address[0].family == AF_INET) ? 1 : 28, 1, 120,
			(cfg.address[0].family == AF_INET) ? 4 : 16);
	if(bytes < 0){
		return 1;
	}
	offset += bytes;

	memcpy(frame + offset, cfg.address[0].addr, (cfg.address[0].family == AF_INET) ? 4 : 16);
	offset += (cfg.address[0].family == AF_INET) ? 4 : 16;

	//push all other announce addresses with a pointer
	for(u = 1; u < cfg.addresses; u++){
		frame[offset++] = 0xC0 | (host_offset >> 8);
		frame[offset++] = host_offset & 0xFF;
		bytes = dns_push_rr(frame + offset, sizeof(frame) - offset, &rr, (char*) frame + offset,
				(cfg.address[u].family == AF_INET) ? 1 : 28, 1, 120,
				(cfg.address[u].family == AF_INET) ? 4 : 16);
		if(bytes < 0){
			return 1;
		}
		offset += bytes;

		memcpy(frame + offset, cfg.address[u].addr, (cfg.address[u].family == AF_INET) ? 4 : 16);
		offset += (cfg.address[u].family == AF_INET) ? 4 : 16;
	}

	data->last_announce = mm_timestamp();
	free(name.name);
	return rtpmidi_mdns_broadcast(frame, offset);
bail:
	free(name.name);
	return 1;
}

static int rtpmidi_service(){
	size_t n, u, p;
	instance** inst = NULL;
	rtpmidi_instance_data* data = NULL;
	struct sockaddr_storage control_peer;

	//prepare commands
	apple_sync_frame sync = {
		.res1 = 0xFFFF,
		.command = htobe16(apple_sync),
		.ssrc = 0,
		.count = 0,
		.timestamp = {
			htobe64(mm_timestamp() * 10)  // big-endian
		}
	};

	if(mm_backend_instances(BACKEND_NAME, &n, &inst)){
		LOG("Failed to fetch instances");
		return 1;
	}

	for(u = 0; u < n; u++){
		data = (rtpmidi_instance_data*) inst[u]->impl;

		if(data->mode == apple){
			//mdns discovery
			if((cfg.mdns_fd >= 0 || cfg.mdns4_fd >= 0)
					&& (!data->last_announce || mm_timestamp() - data->last_announce > RTPMIDI_ANNOUNCE_INTERVAL)){
				rtpmidi_mdns_announce(inst[u]);
			}

			for(p = 0; p < data->peers; p++){
				if(data->peer[p].active && data->peer[p].connected){
					//apple sync
					DBGPF("Instance %s initializing sync on peer %" PRIsize_t, inst[u]->name, p);
					sync.ssrc = htobe32(data->ssrc);
					//calculate remote control port from data port
					memcpy(&control_peer, &(data->peer[u].dest), sizeof(control_peer));
					((struct sockaddr_in*) &control_peer)->sin_port = htobe16(be16toh(((struct sockaddr_in*) &control_peer)->sin_port) - 1);

					if(sendto(data->control_fd, (char*) &sync, sizeof(apple_sync_frame), 0, (struct sockaddr*) &control_peer, data->peer[u].dest_len) != sizeof(apple_sync_frame)){
						LOG("Failed to output sync frame");
					}
				}
				else if(data->peer[p].active && !data->peer[p].learned && (mm_timestamp() / 1000) % 10 == 0){
					//try to invite pre-defined unconnected applemidi peers
					DBGPF("Instance %s inviting configured peer %" PRIsize_t, inst[u]->name, p);
					rtpmidi_peer_applecommand(inst[u], p, 1, apple_invite, 0);
				}
			}
		}
	}

	free(inst);
	return 0;
}

static int rtpmidi_apple_peermatch(uint8_t* session_raw, struct sockaddr* peer, socklen_t peer_len, uint16_t control_port){
	//due to mdns restrictions, session names can at most be 255 characters long
	char session_name[1024] = "";
	rtpmidi_instance_data* data = NULL;
	size_t u, n, p;
	uint8_t done = 0;

	//modify peer to match the data port for the indicated control port
	((struct sockaddr_in*) peer)->sin_port = htobe16(control_port + 1);
	snprintf(session_name, sizeof(session_name), "%.*s", session_raw[0], session_raw + 1);

	//find instances that invite exactly this peer
	for(u = 0; u < cfg.invites; u++){
		for(n = 0; n < cfg.invite[u].invites; n++){
			if(strlen(cfg.invite[u].name[n]) == session_raw[0]
					&& !strcmp(cfg.invite[u].name[n], session_name)){
				done = 1;
				data = (rtpmidi_instance_data*) cfg.invite[u].inst->impl;
				DBGPF("Peer %s explicitly invited on instance %s", session_name, cfg.invite[u].inst->name);

				//check whether this peer (or its equivalent on another protocol) is already connected
				for(p = 0; p < data->peers; p++){
					//FIXME might want to scan for explicitly configured peers that match the announced peer
					if(data->peer[p].active
							&& data->peer[p].learned
							&& data->peer[p].invite == n){
						//we already learned of this peer
						break;
					}
				}

				if(p == data->peers){
					//push a new peer
					if(rtpmidi_push_peer(data, peer, peer_len, 1, 0, n)){
						return 1;
					}
					//find it again
					for(p = 0; p < data->peers; p++){
						if(data->peer[p].active
								&& data->peer[p].learned
								&& data->peer[p].invite == n){
							//we already learned of this peer
							break;
						}
					}
				}
				else{
					//if connected, we're done for this instance
					//if not, at least the family should match
					if(data->peer[p].connected
							|| data->peer[p].dest.ss_family != peer->sa_family){
						break;
					}

					//if not connected and family matches, overwrite
					memcpy(&(data->peer[p].dest), peer, data->peer[p].dest_len);
				}

				//connect either the pushed or overwritten peer
				LOGPF("Inviting peer %s to instance %s", session_name, cfg.invite[u].inst->name);
				rtpmidi_peer_applecommand(cfg.invite[u].inst, p, 1, apple_invite, 0);
			}
		}
	}

	//if we found at least one match before, we don't check wildcard invites
	if(done){
		return 0;
	}

	//find instances with a wildcard invite
	for(u = 0; u < cfg.invites; u++){
		for(n = 0; n < cfg.invite[u].invites; n++){
			if(!strcmp(cfg.invite[u].name[n], "*")){
				done = 1;
				DBGPF("Peer %s implicitly invited on instance %s, converting to explicit invitation", session_name, cfg.invite[u].inst->name);
				if(rtpmidi_push_invite(cfg.invite[u].inst, session_name)){
					return 1;
				}
			}
		}
	}

	//recurse to connect now-explicit invitations
	if(done){
		rtpmidi_apple_peermatch(session_raw, peer, peer_len, control_port);
	}
	return 0;
}

//TODO bounds check all accesses
static int rtpmidi_parse_announce(uint8_t* buffer, size_t length, dns_header* hdr, dns_name* name, dns_name* host, struct sockaddr* source, socklen_t source_len){
	dns_rr* rr = NULL;
	dns_rr_srv* srv = NULL;
	size_t u = 0, offset = sizeof(dns_header);
	uint8_t* session_name = NULL;
	char peer_name[1024];

	for(u = 0; u < hdr->questions; u++){
		if(dns_decode_name(buffer, length, offset, name)){
			LOG("Failed to decode DNS label");
			return 1;
		}
		offset += name->length;
		offset += sizeof(dns_question);
	}

	//look for a SRV answer for ._apple-midi._udp.local.
	for(u = 0; u < hdr->answers; u++){
		if(dns_decode_name(buffer, length, offset, name)){
			LOG("Failed to decode DNS label");
			return 1;
		}

		//store a pointer to the first label in the current path
		//since we decoded the name successfully before and dns_decode_name performs bounds checking, this _should_ be ok
		session_name = (DNS_POINTER(buffer[offset])) ? buffer + (DNS_LABEL_LENGTH(buffer[offset]) << 8 | buffer[offset + 1]) : buffer + offset;

		offset += name->length;
		rr = (dns_rr*) (buffer + offset);
		offset += sizeof(dns_rr);

		if(be16toh(rr->rtype) == 33
				&& strlen(name->name) > strlen(RTPMIDI_MDNS_DOMAIN)
				&& !strcmp(name->name + (strlen(name->name) - strlen(RTPMIDI_MDNS_DOMAIN)), RTPMIDI_MDNS_DOMAIN)){
			//decode the srv data
			srv = (dns_rr_srv*) (buffer + offset);
			offset += sizeof(dns_rr_srv);

			if(dns_decode_name(buffer, length, offset, host)){
				LOG("Failed to decode SRV target");
				return 1;
			}

			if(!strncmp(host->name, cfg.mdns_name, strlen(cfg.mdns_name)) && host->name[strlen(cfg.mdns_name)] == '.'){
				//ignore loopback packets, we don't care about them
				return 0;
			}

			//we just use the packet's source as peer, because who would announce mdns for another host (also implementing an additional registry for this would bloat this backend further)
			LOGPF("Detected possible peer %.*s on %s (%s) Port %d", session_name[0], session_name + 1, host->name, mmbackend_sockaddr_ntop(source, peer_name, sizeof(peer_name)), be16toh(srv->port));
			offset -= sizeof(dns_rr_srv);

			rtpmidi_apple_peermatch(session_name, source, source_len, be16toh(srv->port));
		}

		offset += be16toh(rr->data);
	}


	return 0;
}

static int rtpmidi_handle_mdns(int fd){
	uint8_t buffer[RTPMIDI_PACKET_BUFFER];
	dns_header* hdr = (dns_header*) buffer;
	dns_name name = {
		.alloc = 0
	}, host = name;
	ssize_t bytes = 0;
	struct sockaddr_storage peer_addr;
	socklen_t peer_len = sizeof(peer_addr);
	#ifdef DEBUG
	char peer_name[INET6_ADDRSTRLEN + 1];
	#endif

	for(bytes = recvfrom(fd, buffer, sizeof(buffer), 0, (struct sockaddr*) &peer_addr, &peer_len);
			bytes > 0;
			bytes = recvfrom(fd, buffer, sizeof(buffer), 0, (struct sockaddr*) &peer_addr, &peer_len)){
		if(bytes < sizeof(dns_header)){
			continue;
		}

		//decode basic header
		hdr->id = be16toh(hdr->id);
		hdr->questions = be16toh(hdr->questions);
		hdr->answers = be16toh(hdr->answers);
		hdr->servers = be16toh(hdr->servers);
		hdr->additional = be16toh(hdr->additional);

		//rfc6762 18.3: opcode != 0 -> ignore
		//rfc6762 18.11: response code != 0 -> ignore

		DBGPF("%" PRIsize_t " bytes on v%c, ID %d, Opcode %d, %s, %d questions, %d answers, %d servers, %d additional, src %s",
				bytes, (fd == cfg.mdns_fd ? '6' : '4'), hdr->id,
				DNS_OPCODE(hdr->flags[0]), DNS_RESPONSE(hdr->flags[0]) ? "response" : "query",
				hdr->questions, hdr->answers, hdr->servers, hdr->additional,
				mmbackend_sockaddr_ntop((struct sockaddr*) &peer_addr, peer_name, sizeof(peer_name)));
		rtpmidi_parse_announce(buffer, bytes, hdr, &name, &host, (struct sockaddr*) &peer_addr, peer_len);

		peer_len = sizeof(peer_addr);
	}

	free(name.name);
	free(host.name);
	#ifdef _WIN32
	if(WSAGetLastError() == WSAEWOULDBLOCK){
	#else
	if(errno == EAGAIN){
	#endif
		return 0;
	}

	LOGPF("Error reading from mDNS descriptor: %s", mmbackend_socket_strerror(errno));
	return 1;
}

static int rtpmidi_handle(size_t num, managed_fd* fds){
	size_t u;
	int rv = 0;
	instance* inst = NULL;
	rtpmidi_instance_data* data = NULL;

	//handle service tasks (mdns, clock sync, peer connections)
	if(mm_timestamp() - cfg.last_service > RTPMIDI_SERVICE_INTERVAL){
		//DBGPF("Performing service tasks, delta %" PRIu64, mm_timestamp() - cfg.last_service);
		if(rtpmidi_service()){
			return 1;
		}
		cfg.last_service = mm_timestamp();
	}

	for(u = 0; u < num; u++){
		if(!fds[u].impl){
			//handle mDNS discovery input
			rtpmidi_handle_mdns(fds[u].fd);
		}
		else{
			//handle rtp/control input
			inst = (instance*) fds[u].impl;
			data = (rtpmidi_instance_data*) inst->impl;
			if(fds[u].fd == data->fd){
				rv |= rtpmidi_handle_data(inst);
			}
			else if(fds[u].fd == data->control_fd){
				rv |= rtpmidi_handle_control(inst);
			}
			else{
				LOG("Signaled for unknown descriptor");
			}
		}
	}

	return rv;
}

static int rtpmidi_start_mdns(){
	//use ip_mreqn where possible, but that renames the interface member
	#ifdef _WIN32
	struct ip_mreq mcast_req = {
		.imr_interface.s_addr = INADDR_ANY,
	#else
	struct ip_mreqn mcast_req = {
		.imr_address.s_addr = INADDR_ANY,
	#endif
		.imr_multiaddr.s_addr = htobe32(((uint32_t) 0xe00000fb))
	};

	struct ipv6_mreq mcast6_req = {
		.ipv6mr_multiaddr.s6_addr = {0xff, 0x02, 0x00, 0x00,
						0x00, 0x00, 0x00, 0x00,
						0x00, 0x00, 0x00, 0x00,
						0x00, 0x00, 0x00, 0xfb},
		.ipv6mr_interface = 0
	};

	if(!cfg.mdns_name){
		LOG("No mDNS name set, disabling AppleMIDI discovery");
		return 0;
	}

	if(cfg.mdns_interface){
		#ifdef _WIN32
		mcast6_req.ipv6mr_interface = cfg.mdns6_adapter;
		mcast_req.imr_interface.s_addr = htobe32(cfg.mdns_adapter);
		#else
		mcast6_req.ipv6mr_interface = if_nametoindex(cfg.mdns_interface);
		mcast_req.imr_ifindex = if_nametoindex(cfg.mdns_interface);
		#endif
	}

	//FIXME might try passing NULL as host here to work around possible windows ipv6 handicaps
	cfg.mdns_fd = mmbackend_socket(RTPMIDI_DEFAULT_HOST, RTPMIDI_MDNS_PORT, SOCK_DGRAM, 1, 1, 0);
	cfg.mdns4_fd = mmbackend_socket(RTPMIDI_DEFAULT4_HOST, RTPMIDI_MDNS_PORT, SOCK_DGRAM, 1, 1, 0);
	if(cfg.mdns_fd < 0 && cfg.mdns4_fd < 0){
		LOG("Failed to create requested mDNS descriptors");
		return 1;
	}

	//join ipv4 multicast group
	if(cfg.mdns4_fd >= 0 && setsockopt(cfg.mdns4_fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, (uint8_t*) &mcast_req, sizeof(mcast_req))){
		LOGPF("Failed to join IPv4 multicast group for mDNS, discovery may be impaired: %s", mmbackend_socket_strerror(errno));
	}

	//join ipv6 multicast group
	if(cfg.mdns_fd >= 0 && setsockopt(cfg.mdns_fd, IPPROTO_IPV6, IPV6_ADD_MEMBERSHIP, (uint8_t*) &mcast6_req, sizeof(mcast6_req))){
		LOGPF("Failed to join IPv6 multicast group for mDNS, discovery may be impaired: %s", mmbackend_socket_strerror(errno));
	}

	//register mdns fd to core
	return mm_manage_fd(cfg.mdns_fd, BACKEND_NAME, 1, NULL) | mm_manage_fd(cfg.mdns4_fd, BACKEND_NAME, 1, NULL);
}

static int rtpmidi_start(size_t n, instance** inst){
	size_t u, p, fds = 0;
	rtpmidi_instance_data* data = NULL;
	uint8_t mdns_requested = 0;

	for(u = 0; u < n; u++){
		data = (rtpmidi_instance_data*) inst[u]->impl;
		//check whether instances are explicitly configured to a mode
		if(data->mode == unconfigured){
			LOGPF("Instance %s is missing a mode configuration", inst[u]->name);
			return 1;
		}

		//generate random ssrc's
		if(!data->ssrc){
			data->ssrc = ((uint32_t) rand()) << 16 | rand();
		}

		//if not bound, bind to default
		if(data->fd < 0 && rtpmidi_bind_instance(inst[u], data, RTPMIDI_DEFAULT_HOST, NULL)){
			LOGPF("Failed to bind default sockets for instance %s", inst[u]->name);
			return 1;
		}

		//mark configured peers on direct instances as connected so output is sent
		//apple mode instances go through the session negotiation before marking peers as active
		if(data->mode == direct){
			for(p = 0; p < data->peers; p++){
				data->peer[p].connected = 1;
			}
		}
		else if(data->mode == apple){
			mdns_requested = 1;
		}

		//register fds to core
		if(mm_manage_fd(data->fd, BACKEND_NAME, 1, inst[u]) || (data->control_fd >= 0 && mm_manage_fd(data->control_fd, BACKEND_NAME, 1, inst[u]))){
			LOGPF("Failed to register descriptor for instance %s with core", inst[u]->name);
			return 1;
		}
		fds += (data->control_fd >= 0) ? 2 : 1;
	}

	if(mdns_requested && (rtpmidi_announce_addrs() || rtpmidi_start_mdns())){
		LOG("Failed to set up mDNS discovery, instances may not show up on remote hosts and may not find remote peers");
	}
	else if(mdns_requested){
		fds += 2;
	}

	LOGPF("Registered %" PRIsize_t " descriptors to core", fds);
	return 0;
}

static int rtpmidi_shutdown(size_t n, instance** inst){
	rtpmidi_instance_data* data = NULL;
	size_t u, p;

	for(u = 0; u < n; u++){
		data = (rtpmidi_instance_data*) inst[u]->impl;

		if((cfg.mdns_fd >= 0 || cfg.mdns4_fd >= 0) && data->mode == apple){
			rtpmidi_mdns_detach(inst[u]);
		}

		if(data->fd >= 0){
			close(data->fd);
		}

		if(data->control_fd >= 0){
			close(data->control_fd);
		}

		free(data->accept);
		data->accept = NULL;

		free(data->peer);
		data->peer = NULL;
		data->peers = 0;

		free(inst[u]->impl);
		inst[u]->impl = NULL;
	}

	for(u = 0; u < cfg.invites; u++){
		for(p = 0; p < cfg.invite[u].invites; p++){
			free(cfg.invite[u].name[p]);
		}
		free(cfg.invite[u].name);
	}
	free(cfg.invite);
	cfg.invite = NULL;
	cfg.invites = 0;

	free(cfg.address);
	cfg.addresses = 0;

	free(cfg.mdns_name);
	cfg.mdns_name = NULL;
	free(cfg.mdns_interface);
	cfg.mdns_interface = NULL;
	if(cfg.mdns_fd >= 0){
		close(cfg.mdns_fd);
	}
	if(cfg.mdns4_fd >= 0){
		close(cfg.mdns4_fd);
	}

	LOG("Backend shut down");
	return 0;
}
