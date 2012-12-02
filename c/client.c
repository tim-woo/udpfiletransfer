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

#define MAXBUFLEN 1024
#define MAXSEQNUMS 256
#define REQ 'R'
#define ACK 'A'

int noPacketLoss(int probloss) {
	int r = rand()%100;
	if (r < probloss) {
		printf("ACK lost\n");
		return 0;	// packet loss
	}
	else {
		return 1;	// no packet loss
	}
}

int notCorrupt(int probcorrupt) {
	int r = rand()%100;
	if (r < probcorrupt) {
		printf("Packet received is corrupt\n");
		return 0;	// corrupted file
	}
	else {
		return 1;	// not corrupt file
	}
}

int hasSeqNum(char* rcvpkt, unsigned char expectedSequenceNum) {
	printf("seq: %d, expected: %d\n", (unsigned char)rcvpkt[0], expectedSequenceNum);
	if ((unsigned char)rcvpkt[0] == expectedSequenceNum)
		return 1;
	else
		return 0;
}

int main(int argc, char *argv[])
{
	int sockfd;
	struct addrinfo hints, *servinfo, *p;
	int rv;
	int numbytes;
	char fname[MAXBUFLEN];
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
	bzero(fname, MAXBUFLEN);
	fname[0] = REQ;
	fname[1] = '\0';
	strcat(fname, argv[3]);

	if ((numbytes = sendto(sockfd, fname, strlen(argv[3])+1, 0,
			 p->ai_addr, p->ai_addrlen)) == -1) {
		perror("talker: sendto");
		exit(1);
	}
	printf("talker: sent %d bytes to %s\n", numbytes, argv[1]);

	// CREATE NEW FILE
	if ((fd = open(argv[3], O_WRONLY | O_CREAT | O_APPEND, 0666)) < 0) {
		perror("Error Creating File");
		exit(1);
	}
 
    if (fd < 0) {
        return -1;
    }

	int numwrite;

	// PREPARE FOR GO-BACK-N PROTOCOL
	unsigned char expectedSeqNum = 0;
	int addr_len;
	int last_packet = 0;
	char buf[MAXBUFLEN];
	addr_len = sizeof(serv_addr);

	printf("talker: waiting to recvfrom...\n");

	for (;;) {
		bzero(buf, MAXBUFLEN);

		// Block and wait for server to send packet
		if ((numbytes = recvfrom(sockfd, buf, MAXBUFLEN , 0,
			NULL, NULL)) == -1) {
			perror("recvfrom");
			exit(1);
		}

		if (notCorrupt(probcorrupt) && hasSeqNum(buf, expectedSeqNum)) {
			if ((int)buf[1] == 1) {
				printf("recv: last_packet\n");
				last_packet = 1;
			}

			if (!last_packet) {
				if (write(fd, (char *)buf+2, MAXBUFLEN-2) < 0) {
					perror("write error");
					exit(1);
				}
			} 
			else {
				char c;
				int i = 2;
				while (*(buf+i) != 0) {
					c = *(buf+i);
					write(fd,(char *)buf+i,1);
					i++;
				}
			}

			printf("recv: data packet seq # %d\n", (unsigned char)buf[0]);

			// Make packet to send back
			bzero(buf, MAXBUFLEN);
			buf[0] = ACK; // PACKET TYPE
			buf[1] = expectedSeqNum; // ACK SEQ NUM

			if (noPacketLoss(probloss) == 1) {
			 	if ((numbytes = sendto(sockfd, buf, MAXBUFLEN, 0,
						 p->ai_addr, p->ai_addrlen)) == -1) {
					perror("talker: sendto");
					exit(1);
				}
			} else {
				printf("**ACK # %d was sent but lost**\n", expectedSeqNum);
			}

			printf("sent: ACK # %d\n", expectedSeqNum);

			expectedSeqNum = (expectedSeqNum + 1) % MAXSEQNUMS;

			if (last_packet) {
				break;
			}
			
		} else {
			printf("**Packet with seq # %d CORRUPTED**\n", (unsigned char)buf[0]);
			bzero(buf, MAXBUFLEN);
		}
		
	}

	freeaddrinfo(servinfo);
	close(sockfd);
	close(fd);

	return 0;
}
