/*
** server.c -- a datagram sockets "server" demo
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

#include <signal.h>
#include <sys/stat.h>


// #define MYPORT "4950"	// the port users will be connecting to

#define PACKET_SIZE 1000
#define HEADER_SIZE 9	// 4 bytes Seq Num, 4 byte Packet Payload size, 1 byte Last Packet
#define PAYLOAD_SIZE 991

#define MAXSEQNUMS 4294967000 // 2^32-1 floor 1000
#define TIMEOUT 2

typedef enum fileType {ACK,REQ} fileType;

int sockfd, sendfd;
unsigned int nextSeqNum, numPackets, total_payload_sent, base, file_base, fsize, cwnd;
char* portno;
int finished, probloss, probcorrupt;
fileType expectedType;
struct sockaddr_in their_addr;

void io_handler(int signal);
void catch_alarm(int signal);

int checkNextSeq(int nextSeq, int base, int cwnd) {
// TODO:::::::::::::::::::::::::::::::::::::::::::::::::::::::: fix for uneven window in bytes where it doesnt go into packet size

	if ((base+cwnd)%MAXSEQNUMS != base+cwnd) {		// Window split
		if ( (nextSeq > base && nextSeq > ((base+cwnd)%MAXSEQNUMS)) ||
			(nextSeq < base && nextSeq<(base+cwnd)%MAXSEQNUMS)) {
			return 1;
		}
	}
	else {											// Window not split
		if (nextSeq<base+cwnd && nextSeq+PACKET_SIZE <= base+cwnd) { // fits in window
			return 1;
		}
	}
	return 0;
}

int noPacketLoss() {
	int r = rand()%100;
	if (r < probloss) {
		// printf("sent packet loss\n");
		return 0;	// packet loss
	}
	else {
		return 1;	// no packet loss
	}
}

int notCorrupt() {
	int r = rand()%100;
	if (r < probcorrupt) {
		// printf("ACK corrupted\n");
		return 0;	// corrupted file
	}
	else {
		return 1;	// not corrupt file
	}
}

// get sockaddr, IPv4 or IPv6:
void *get_in_addr(struct sockaddr *sa)
{
	if (sa->sa_family == AF_INET) {
		return &(((struct sockaddr_in*)sa)->sin_addr);
	}

	return &(((struct sockaddr_in6*)sa)->sin6_addr);
}

int main(int argc, char* argv[])
{
	struct addrinfo hints, *servinfo, *p;
	int rv;
	
	socklen_t addr_len;		// address length
	char s[INET6_ADDRSTRLEN];		// IPv6 variables
    char ipstr[INET6_ADDRSTRLEN];

    if (argc != 5) {
		fprintf(stderr,"usage: server portnumber cwnd probloss probcorrupt\n");
		exit(1);
    }

    portno = argv[1];
    cwnd = atoi(argv[2]);
    probloss = atoi(argv[3]);
    probcorrupt = atoi(argv[4]);

    if (cwnd < PACKET_SIZE) {
    	fprintf(stderr, "Window size (bytes) must be larger than the preset packet size (1000 bytes)\n");
    }

    // simulate packet loss
    srand((unsigned)time(NULL));

    // Set hints for DNS lookup
	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_INET;//AF_UNSPEC; // set to AF_INET to force IPv4
	hints.ai_socktype = SOCK_DGRAM;
	hints.ai_flags = AI_PASSIVE; // use my IP (AI_PASSIVE  tells getaddrinfo() to assign the address of my local host to the socket structures)

	// DNS lookup
	if ((rv = getaddrinfo(NULL, portno, &hints, &servinfo)) != 0) {
		fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
		return 1;
	}

	// loop through all the results and bind to the first we can
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
			perror("listener: socket");
			continue;
		}

		if (bind(sockfd, p->ai_addr, p->ai_addrlen) == -1) {
			close(sockfd);
			perror("listener: bind");
			continue;
		}

		break;
	}

	if (p == NULL) {
		fprintf(stderr, "listener: failed to bind socket\n");
		return 2;
	}

	freeaddrinfo(servinfo);

	//SIGNAL SETUP
	signal(SIGALRM, catch_alarm);

	signal(SIGIO, io_handler);
	// struct sigaction handler;
	// handler.sa_handler = io_handler;
	// if (sigfillset(&handler.sa_mask) < 0) {
	// 	perror("sigfillset");
	// 	exit(1);
	// }

	if (fcntl(sockfd, F_SETOWN, getpid()) < 0) {
		perror("fcntl F_SETOWN");
		exit(1);
	}

	if (fcntl(sockfd, F_SETFL, FASYNC) < 0) {
		perror("fcntl F_SETFL, FASYNC");
		exit(1);
	}


	printf("waiting to recvfrom...\n");


	nextSeqNum = 0;			// in terms of bytes
	base = 0;				// in terms of bytes
	file_base = 0;			// in terms of bytes
	expectedType = REQ;
	finished = 0;
	total_payload_sent = 0;			// keeps running count of bytes sent, reset to file_base when timeout occurs

	int sb = 0;
	int bytesread = 0;
	char fbuf[PACKET_SIZE];	// file name buffer
	bzero(fbuf, PACKET_SIZE);
	
	for (;;) {
		// if (expectedType == ACK && nextSeqNum < base + cwnd) {
		if (expectedType == ACK && checkNextSeq(nextSeqNum,base,cwnd)==1) {
			if ((bytesread = read(sendfd, fbuf + HEADER_SIZE, PAYLOAD_SIZE)) > 0) {
				memcpy(fbuf, (char *)&nextSeqNum, 4);
				memcpy(fbuf+4, &bytesread, 4);
				fbuf[8] = (char)((fsize == total_payload_sent+HEADER_SIZE+bytesread) ? 1 : 0);
				// fbuf[8] = (char)((packet_no + 1) == numPackets ? 1 : 0);

				if (noPacketLoss()) {
					sb = sendto(sockfd, fbuf, HEADER_SIZE + bytesread, 0,
						(struct sockaddr *)&their_addr, sizeof(their_addr));
					printf("sent: SEQ # %d\n", nextSeqNum);
				} else {
					printf("**Packet with SEQ # %d was sent but lost**\n", nextSeqNum);
				}

				printf("\n(bytes read: %d 	total_payload_sent: %d 	fsize: %d nextSeqNum: %d)\n", bytesread, total_payload_sent, fsize, nextSeqNum);

				
				if (base == nextSeqNum) {
					alarm(TIMEOUT);
				}
				nextSeqNum = (nextSeqNum + HEADER_SIZE + bytesread) % MAXSEQNUMS;
				total_payload_sent += bytesread;
				
				bzero(fbuf, PACKET_SIZE);
			} else { 
				if (finished == 1) {
					break;
				}
			}
		}
	}

	close(sendfd);
	// close(sockfd);

	return 0;
}


void io_handler(int sig)
{
	if (sig != SIGIO) {
		printf("Recieved unrecognized signal\n");
		return;
	}
	char pbuf[PACKET_SIZE];
	char* fname;
	int addr_len;
	unsigned int ack_num;
	int numbytes;
	struct stat st;

	bzero(pbuf, PACKET_SIZE);
	addr_len = sizeof(their_addr);
	if ((numbytes = recvfrom(sockfd, pbuf, PACKET_SIZE, 0,
		(struct sockaddr *)&their_addr, &addr_len)) == -1) {
		perror("recvfrom");
		exit(1);
	} else {
		fileType type = (pbuf[0]=='R' ? REQ : ACK);
		if (type == REQ) {
			fname = pbuf + 1;
			sendfd = open(fname, O_RDONLY);
			if (sendfd == -1) {
				perror("No file found");
				exit(1);
			}

			// Get number of total packets
			stat(fname, &st);
			fsize = st.st_size;

			// Allow for sending of file
			expectedType = ACK;
		}
		if (type == ACK) {
			memcpy(&ack_num, pbuf+1,4);
			if (notCorrupt()) {
				// ack_num = (unsigned char)pbuf[1];
				printf("			recv: ACK # %d", ack_num);
				int cnt = 0;
				unsigned int tmp = base;
				base = (ack_num + PACKET_SIZE) % MAXSEQNUMS;	// Cumulative ACK
				
				printf("	new base: %d", base);

				// calculate new file position base
				while (tmp != base) {
					cnt++;				// count number of packets that have been received
					tmp = (tmp + PACKET_SIZE) % MAXSEQNUMS;
				}

				file_base += cnt * PAYLOAD_SIZE;	// increase base file position by that # of packet's payload size

				printf("	file position: %d\n", file_base);

				if (file_base >= fsize) {
					finished = 1;	
				}		

				if (base == nextSeqNum) {
					alarm(0);
				} else {
					alarm(TIMEOUT);
				}	
			}
			else {
				printf("** RCV: ACK # %d CORRUPTED**\n", ack_num);
			}
		}
	}
}

void catch_alarm(int sig)
{
	if (sig != SIGALRM) {
		printf("Recieved unrecognized signal\n");
		return;
	}
	printf("**Packet with seq # %d TIMEOUT**\n", base);

	// Reset alarm
	alarm(TIMEOUT);

	// Seek to base packet's position in file
	lseek(sendfd, (off_t)file_base, SEEK_SET);

	// Reset variables
	total_payload_sent = file_base; // restarts sending from file_base
	nextSeqNum = base;
}