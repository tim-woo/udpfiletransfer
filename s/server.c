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

#define MAXSEQNUMS 256
#define TIMEOUT 2

typedef enum fileType {ACK,REQ} fileType;

int sockfd, sendfd;
unsigned int nextSeqNum, numPackets, packet_no;
int base, fpos, fsize;
int cwnd, probloss, probcorrupt;
char* portno;
int finished;
fileType expectedType;
struct sockaddr_in their_addr;

void io_handler(int signal);
void catch_alarm(int signal);

int checkNextSeq(int nextSeq, int base, int cwnd) {
	if ((base+cwnd)%MAXSEQNUMS != base+cwnd) {
		if ( (nextSeq > base && nextSeq > ((base+cwnd)%MAXSEQNUMS)) ||
			(nextSeq < base && nextSeq<(base+cwnd)%MAXSEQNUMS)) {
			return 1;
		}
	}
	else {
		if (nextSeq<base+cwnd) {
			return 1;
		}
	}
	return 0;
}

int noPacketLoss() {
	int r = rand()%100;
	if (r < probloss) {
		printf("sent packet loss\n");
		return 0;	// packet loss
	}
	else {
		return 1;	// no packet loss
	}
}

int notCorrupt() {
	int r = rand()%100;
	if (r < probcorrupt) {
		printf("ACK corrupted\n");
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
	int numbytes;
	
	char fbuf[MAXBUFLEN];	// file name buffer
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


	nextSeqNum = 0;
	base = 0;
	fpos = 0;
	expectedType = REQ;
	finished = 0;
	packet_no = 0;

	int sb = 0;
	int bytesread = 0;

	bzero(fbuf, MAXBUFLEN);
	
	// i=0;

	for (;;) {
		// if (expectedType == ACK && nextSeqNum < base + cwnd) {
		if (expectedType == ACK && checkNextSeq(nextSeqNum,base,cwnd)==1) {
			fbuf[0] = (char)nextSeqNum;		
			fbuf[1] = (char)((packet_no + 1) == numPackets ? 1 : 0);
			// TODO: Always MAXBUFLEN-2 bytes read?
			if ((bytesread = read(sendfd, fbuf+2, MAXBUFLEN-2)) > 0) {
				if (noPacketLoss()) {
					// sb = sendto(sockfd, fbuf, MAXBUFLEN, 0,
					sb = sendto(sockfd, fbuf, bytesread+2, 0,
						(struct sockaddr *)&their_addr, sizeof(their_addr));
					printf("sent: packet # %d, seq # %d\n", packet_no, nextSeqNum);
				} else {
					printf("**Packet %d, seq # %d was sent but lost**\n", packet_no, nextSeqNum);
				}
				
				if (base == nextSeqNum) {
					alarm(TIMEOUT);
				}

				nextSeqNum = (nextSeqNum + 1) % MAXSEQNUMS;
				packet_no++;
				
				bzero(fbuf, MAXBUFLEN);
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
	char pbuf[MAXBUFLEN];
	char* fname;
	int addr_len;
	int ack_num;
	int numbytes;
	struct stat st;

	bzero(pbuf, MAXBUFLEN);
	addr_len = sizeof(their_addr);
	if ((numbytes = recvfrom(sockfd, pbuf, MAXBUFLEN, 0,
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
			numPackets = (fsize / (MAXBUFLEN - 2)) + (((fsize % (MAXBUFLEN - 2)) != 0) ? 1 : 0);

			// Allow for sending of file
			expectedType = ACK;
		}
		else if (notCorrupt() && type == ACK) {
			ack_num = (unsigned char)pbuf[1];
			printf("recv: ACK # %d\n", ack_num);
			int cnt = 0;
			int tmp = base;
			base = (ack_num + 1) % MAXSEQNUMS;
			
			while (tmp != base) {
				cnt++;
				tmp = (tmp + 1) % MAXSEQNUMS;
			}

			fpos += cnt * (MAXBUFLEN - 2);
			if (fpos >= fsize) {
				finished = 1;	
			}		

			if (base == nextSeqNum) {
				alarm(0);
			} else {
				alarm(TIMEOUT);
			}
		}
		else if(!notCorrupt()) {
			printf("**ACK # %d CORRUPTED**\n", (unsigned char)pbuf[1]);
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
	int i;
	off_t offset = (off_t)fpos;

	// Reset alarm
	alarm(TIMEOUT);

	// Seek to base packet's position in file
	lseek(sendfd, offset, SEEK_SET);

	// Redefine variables
	packet_no = fpos/(MAXBUFLEN-2);
	nextSeqNum = base;
}