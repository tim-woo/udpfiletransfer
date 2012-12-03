/*
** client.c -- a datagram "client" demo
*/

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#include <fcntl.h>

#define PACKET_SIZE 1000
#define HEADER_SIZE 9	// 4 bytes Seq Num, 4 byte Packet Payload size, 1 byte Last Packet
#define PAYLOAD_SIZE 991
#define MAXSEQNUMS 4294967000 // 2^32-1 floor 1000
#define REQ 'R'
#define ACK 'A'

int noPacketLoss(int probloss) {
	int r = rand()%100;
	if (r < probloss) {
		// printf("ACK lost\n");
		return 0;	// packet loss
	}
	else {
		return 1;	// no packet loss
	}
}

int notCorrupt(int probcorrupt) {
	int r = rand()%100;
	if (r < probcorrupt) {
		// printf("Packet received is corrupt\n");
		return 0;	// corrupted file
	}
	else {
		return 1;	// not corrupt file
	}
}

int main(int argc, char *argv[])
{
	int sockfd;
	struct addrinfo hints, *servinfo, *p;
	int rv;
	int numbytes;
	char fname[PACKET_SIZE];
	int fd;
	int probloss, probcorrupt;

    char ipstr[INET6_ADDRSTRLEN];

    // Used to simulate packet corruption
    srand((unsigned int)time(NULL));

	if (argc != 6) {
		fprintf(stderr,"usage: client hostname portnumber filename probloss probcorrupt\n");
		exit(1);
	}

	probloss = atoi(argv[4]);
	probcorrupt = atoi(argv[5]);

	if (probcorrupt < 0 || probloss < 0) {
		fprintf(stderr, "Probability X/100 must be greater than 0.\n");
	}

	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_DGRAM;

    // "getaddrinfo" will do the DNS lookup for you
	if ((rv = getaddrinfo(argv[1], argv[2], &hints, &servinfo)) != 0) {
		fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
		return 1;
	}

	// loop through all the results and make a socket
	for(p = servinfo; p != NULL; p = p->ai_next) {
      
        /*************Print Info********************/
        void *addr;
        char *ipver;
        
        // get the pointer to the address itself,
        // different fields in IPv4 and IPv6:
        if (p->ai_family == AF_INET) { // IPv4
            struct sockaddr_in *ipv4 = (struct sockaddr_in *)p->ai_addr;
            addr = &(ipv4->sin_addr);
            ipver = "IPv4";
        } else { // IPv6
            struct sockaddr_in6 *ipv6 = (struct sockaddr_in6 *)p->ai_addr;
            addr = &(ipv6->sin6_addr);
            ipver = "IPv6";
        }
        
        // convert the IP to a string and print it:
        inet_ntop(p->ai_family, addr, ipstr, sizeof ipstr);
        printf("  %s: %s\n", ipver, ipstr);
        /************************************/
        
		if ((sockfd = socket(p->ai_family, p->ai_socktype,
				p->ai_protocol)) == -1) {
			perror("talker: socket");
			continue;
		}

		break;
	}

	if (p == NULL) {
		fprintf(stderr, "talker: failed to bind socket\n");
		return 2;
	}

	// FOR BINDING - NOT REALLY NEEDED
	struct sockaddr_in serv_addr;
	bzero((char *) &serv_addr, sizeof(serv_addr));

	// SEND FILE REQUEST
	bzero(fname, PACKET_SIZE);
	fname[0] = REQ;
	fname[1] = '\0';
	strcat(fname, argv[3]);

	if ((numbytes = sendto(sockfd, fname, strlen(argv[3])+1, 0,
			 p->ai_addr, p->ai_addrlen)) == -1) {
		perror("talker: sendto");
		exit(1);
	}
	printf("sent: Requested file %s from server", argv[3]);

	// CREATE NEW FILE
	if ((fd = open(argv[3], O_WRONLY | O_CREAT | O_APPEND, 0666)) < 0) {
		perror("Error Creating File");
		exit(1);
	}

	// PREPARE FOR GO-BACK-N PROTOCOL
	unsigned int expectedSeqNum = 0;
	int addr_len;
	unsigned int last_packet = 0;
	char buf[PACKET_SIZE];
	bzero(buf, PACKET_SIZE);
	char ackPacket[PACKET_SIZE];
	bzero(ackPacket, PACKET_SIZE);
	addr_len = sizeof(serv_addr);

	printf("Waiting for response...\n");

	for (;;) {
		// Block and wait for server to send packet
		if ((numbytes = recvfrom(sockfd, buf, PACKET_SIZE , 0,
			NULL, NULL)) == -1) {
			bzero(buf, PACKET_SIZE);
			perror("recvfrom error");
			exit(1);
		}
		unsigned packetSeqNum = 0;
		memcpy(&packetSeqNum, buf, 4);
		if (notCorrupt(probcorrupt)) {
			printf("Expecting Seq: %d - Received Seq %d\n",expectedSeqNum, packetSeqNum);
			if (expectedSeqNum == packetSeqNum) {
				if ((int)buf[8] == 1) {
					printf("recv: last_packet\n");
					last_packet = 1;
				}
				
				unsigned int payload_size = 0;
				memcpy(&payload_size, buf+4, 4);
				unsigned int bytesWrote = 0;
				if (bytesWrote = write(fd, buf+HEADER_SIZE, payload_size) < 0) {
					perror("write error");
					exit(1);
				}

				// Make packet to send back
				ackPacket[0] = ACK; // PACKET TYPE
				memcpy(ackPacket+1, &expectedSeqNum, 4);

				if (noPacketLoss(probloss) == 1) {
				 	if ((numbytes = sendto(sockfd, ackPacket, PACKET_SIZE, 0,
							 p->ai_addr, p->ai_addrlen)) == -1) {
						perror("talker: sendto");
						exit(1);
					}
				} else {
					printf("**ACK # %d was sent but lost**\n", expectedSeqNum);
				}

				printf("sent: ACK # %d\n", expectedSeqNum);

				expectedSeqNum = (expectedSeqNum + HEADER_SIZE+payload_size) % MAXSEQNUMS;
				printf("Set expectedSeqNum to: %d after receiving SEQ: %d payload: %d\n", expectedSeqNum, packetSeqNum, payload_size);

				if (last_packet) {
					break;
				}
				bzero(buf, PACKET_SIZE);
				bzero(ackPacket, PACKET_SIZE);
			}
		} else {
			printf("**Packet with seq # %d CORRUPTED**\n", packetSeqNum);
			bzero(buf, PACKET_SIZE);
		}
		
	}

	freeaddrinfo(servinfo);
	close(sockfd);
	close(fd);

	return 0;
}
