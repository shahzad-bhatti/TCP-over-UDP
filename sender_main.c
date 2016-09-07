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
#include <sys/time.h>

#define MAXBUFLEN 1472
#define FRAMES 60000
#define DATA 0
#define SYN 1
#define SYN_ACK 2
#define ACK 3
#define FIN 4
#define FIN_ACK 5

int64_t timeOut, estimatedRTT = 1000, deviation = 1, difference = 0;
int LAR = -1; // last acknowlegement received
int LFS = -1; // last frame sent
int SWS = 0; // sender window size
uint8_t ACKed[FRAMES];
uint8_t sent[FRAMES];

typedef struct {   
	uint64_t sent_time; //sent time in microseconds for RTT calculations   
	uint16_t seq_no; //sequence number for sender and expected sequence number for receiver
	uint16_t code;   //where Code value can be DATA, SYN, SYN_ACK, ACK, FIN
}TCP_hearder;

uint64_t htonll(uint64_t host_longlong)
{
    int x = 1;
    /* little endian */
    if(*(char *)&x == 1)
        return ((((uint64_t)htonl(host_longlong)) << 32) + htonl(host_longlong >> 32));
    /* big endian */
    else
        return host_longlong;
}
 
uint64_t ntohll(uint64_t host_longlong)
{
    int x = 1;
    /* little endian */
    if(*(char *)&x == 1)
        return ((((uint64_t)ntohl(host_longlong)) << 32) + ntohl(host_longlong >> 32));
    /* big endian */
    else
        return host_longlong;
}

void print_header(TCP_hearder * header)
{
	printf( "Seq_no = %d \n", ntohs(header->seq_no));
	printf( "Code = %d \n", ntohs(header->code));
	printf( "Sent time = %llu\n", (unsigned long long) ntohll(header->sent_time));
}

int set_timeout(int sockfd, int usec)
{
	if (usec < 0)
		return -1;
	struct timeval tv;
	tv.tv_sec = 0;
	tv.tv_usec = usec; 
	if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
    	perror("sender: setsockopt");
	}
	return 0;
}

uint64_t time_now()
{
	struct timeval current;
	gettimeofday(&current, 0);
	return current.tv_sec * 1000000 + current.tv_usec;
}

void update_timeout(uint64_t sentTime)
{
	uint64_t sampleRTT = time_now() - sentTime;
	estimatedRTT = 0.875 * estimatedRTT + 0.125 * sampleRTT; // alpha = 0.875
	deviation += (0.25 * ( abs(sampleRTT - estimatedRTT) - deviation)); //delta = 0.25
	timeOut = (estimatedRTT + 4 * deviation); // mu = 1, phi = 4
	timeOut = timeOut/5;
}

uint16_t win_size()
{
	return 50 * timeOut /MAXBUFLEN;  // (100/8) MBps * timeOut (usec) / MAXBUFLEN 
}

int reliablyTransfer(char* hostname, char * udpPort, char* filename, unsigned long long int bytesToTransfer) 
{
	int sockfd, rv, numbytes, i;
	struct addrinfo hints, *servinfo, *p;
	struct sockaddr_in bind_addr, sendto_addr;
	struct sockaddr their_addr;
	socklen_t their_addr_len = sizeof(their_addr);
	char buf[MAXBUFLEN];

	uint64_t startTime = time_now(); //for throughput calculations at the end
	//setting up sendto address
	uint16_t sendto_port = (uint16_t)atoi(udpPort);
	memset(&sendto_addr, 0, sizeof(sendto_addr));
	sendto_addr.sin_family = AF_INET;
	sendto_addr.sin_port = htons(sendto_port);
	inet_pton(AF_INET, hostname, &sendto_addr.sin_addr);

	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_DGRAM;

	if ((rv = getaddrinfo(hostname, udpPort, &hints, &servinfo)) != 0) {
		fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
		return 1;
	}

	// loop through all the results and bind to the first we can
	for(p = servinfo; p != NULL; p = p->ai_next) {
		if ((sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) == -1) {
			perror("sender: socket");
			continue;
		}
		break;
	}
	if (p == NULL) {
		fprintf(stderr, "sender: failed to create socket\n");
		return 2;
	}
	//extract my port number
	uint16_t my_port = ((struct sockaddr_in *)p->ai_addr )->sin_port;
	
	freeaddrinfo(servinfo);  //don't need it anymore
	set_timeout(sockfd, estimatedRTT); //100ms
	memset(&bind_addr, 0, sizeof(bind_addr));
	bind_addr.sin_family = AF_INET;
	bind_addr.sin_port = htons(my_port);
	bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);

	if (bind(sockfd, (struct sockaddr *)&bind_addr, sizeof(struct sockaddr_in)) == -1) 
	{
		close(sockfd);
		perror("sender: bind");
		exit(1);
	}
	TCP_hearder my_header, their_header;
	int TCP_size = (int) sizeof(my_header);
	my_header.seq_no = 0;
	my_header.code = SYN;
	printf("Starting three way handshake...\n");
	while(1)
	{
		my_header.sent_time = time_now();
		memcpy(buf, &my_header, TCP_size);
		if (numbytes = sendto(sockfd, buf, TCP_size, 0, (struct sockaddr* )&sendto_addr,
			sizeof(sendto_addr)) == -1) {
			perror("sender: sendto");
			exit(1);
		}
		numbytes = recvfrom(sockfd, buf, MAXBUFLEN, 0, &their_addr, &their_addr_len);
		if(numbytes == TCP_size)
		{
			memcpy(&their_header, buf, TCP_size);
			update_timeout(their_header.sent_time);
			break;		
		}
	}
	set_timeout(sockfd, timeOut);
	my_header.code = ACK;
	memcpy(buf, &my_header, TCP_size);
	if (numbytes = sendto(sockfd, buf, TCP_size, 0, (struct sockaddr*)&sendto_addr,
		sizeof(sendto_addr)) == -1) 
	{
		perror("sender: sendto");
		exit(1);
	}
	printf("Connection Established\n");
	int data_len = MAXBUFLEN - TCP_size;
	int segments = bytesToTransfer/data_len;
	int final_seg_size = bytesToTransfer % data_len;
	if (final_seg_size > 0)
		segments++;
	SWS = win_size();
	printf("SWS %d\n", SWS);
	my_header.code = DATA;
	FILE* out_file = fopen(filename, "rb");
	int double_sent = 0;
	printf("Sending File...\n");
	int slow_start = 0;
	uint16_t seq, length;
	int last_LFS = -1;
	int flag = 2;
	uint64_t sentTime[FRAMES] = {0}; 
	uint64_t currTime = 0;
	while(LAR != segments-1)
	{
		if (slow_start < 30)
		{
			SWS = (SWS)/flag;
			slow_start++;
			flag = 3;
		}
		for (i = 0; i < SWS; i++)
		{
			seq = LAR+1+i;
			currTime = time_now();
			if (currTime - sentTime[seq] < timeOut * 5)
				continue;
			if(ACKed[seq] == 0 && seq < segments)
			{
				if (sent[seq] == 1)
					double_sent++;
				sent[seq] = 1;
				my_header.seq_no = seq;
				my_header.sent_time = currTime;
				memcpy(buf, &my_header, TCP_size);
				if (SEEK_CUR != seq * data_len)
					fseek(out_file, data_len * seq, SEEK_SET);
				if (seq == segments-1 && final_seg_size > 0)
					length = final_seg_size;	
				else
					length = data_len;
				fread(buf + TCP_size, 1, length, out_file);
				if (numbytes = sendto(sockfd, buf, length + TCP_size, 0,
			 		(struct sockaddr*)&sendto_addr, sizeof(sendto_addr)) == -1) 
				{	
					perror("sender: sendto");
					exit(1);
				}
				if (LFS < seq)
					LFS = seq;						
			}
			if (last_LFS == LFS)
				set_timeout(sockfd, timeOut * 2);
			last_LFS = LFS;

		}
		while(1)
		{
			numbytes = recvfrom(sockfd, buf, MAXBUFLEN, 0, &their_addr, &their_addr_len);
			if(numbytes == TCP_size)
			{
				memcpy(&their_header, buf, TCP_size);
				if (their_header.code == ACK)
					ACKed[their_header.seq_no] = 1;
				update_timeout(their_header.sent_time);			
			}
			else 
				break;	
		}
		set_timeout(sockfd, timeOut);
		i = LAR;
		i++;
		while(ACKed[i])
			i++;	
		LAR = i-1;
		SWS =win_size();
		if (SWS > 1000)
			SWS = 500;			
	}

	printf("Closing connection...  \n");
	while(their_header.code != FIN_ACK)	
	{
		my_header.code = FIN;
		memcpy(buf, &my_header, TCP_size);
		if (numbytes = sendto(sockfd, buf, TCP_size, 0,
			 (struct sockaddr*)&sendto_addr, sizeof(sendto_addr)) == -1) 
		{	
			perror("sender: sendto");
			exit(1);
		}
		numbytes = recvfrom(sockfd, buf, MAXBUFLEN, 0, &their_addr, &their_addr_len);
		if(numbytes == TCP_size)
			memcpy(&their_header, buf, TCP_size);
	}
	printf("Connection closed \n");
	double timeTaken = (time_now() - startTime)/1000000.0;
	printf("Time taken %f sec\n", timeTaken);
	printf("double sent %d\n", double_sent);
	
	fclose(out_file);
	close(sockfd);
}

int main(int argc, char** argv)
{
	unsigned long long int numBytes;
	if(argc != 5)
	{
		fprintf(stderr, "usage: %s receiver_hostname receiver_port filename_to_xfer bytes_to_xfer\n\n", argv[0]);
		exit(1);
	}
	numBytes = atoll(argv[4]);
	reliablyTransfer(argv[1], argv[2], argv[3], numBytes);
} 
