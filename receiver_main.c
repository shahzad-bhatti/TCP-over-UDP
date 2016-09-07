#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#define MAXBUFLEN 1472
#define FRAMES 60000
#define DATA 0
#define SYN 1
#define SYN_ACK 2
#define ACK 3
#define FIN 4
#define FIN_ACK 5

int NFE = 0, LFA = -1;
uint8_t present[FRAMES];
FILE * recv_file;
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

int reliablyReceive(char * udpPort, char* destinationFile) {
	int sockfd, rv, numbytes, i;
	struct addrinfo hints, *servinfo, *p;
	struct sockaddr their_addr;
	char buf[MAXBUFLEN];
	socklen_t their_addr_len = sizeof(their_addr);

	memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET; // set to AF_INET to force IPv4
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = AI_PASSIVE; // use my IP

    if ((rv = getaddrinfo(NULL, udpPort, &hints, &servinfo)) != 0) {
    	fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
    	return 1;
    }

    // loop through all the results and bind to the first we can
    for(p = servinfo; p != NULL; p = p->ai_next) {
    	if ((sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) == -1) {
    		perror("receiver: socket");
    		continue;
    	}
    	if (bind(sockfd, p->ai_addr, p->ai_addrlen) == -1) {
    		close(sockfd);
    		perror("receiver: bind");
    		continue;
    	}
    	break;
    }

    if (p == NULL) {
    	fprintf(stderr, "receiver: failed to bind socket\n");
    	return 2;
    }
    freeaddrinfo(servinfo);

    printf("receiver: waiting to recvfrom...\n");
    TCP_hearder header;
    int TCP_size = (int) sizeof(header);
    numbytes = recvfrom(sockfd, buf, MAXBUFLEN , 0, (struct sockaddr *)&their_addr, &their_addr_len);
      
    memcpy(&header, buf, TCP_size);
    header.code = SYN_ACK;
	while(1)
	{
		memcpy(buf, &header, TCP_size);
		if ((numbytes = sendto(sockfd, buf, TCP_size, 0, (struct sockaddr *)&their_addr, 
			their_addr_len)) == -1) 
		{
			perror("receiver: sendto");
			exit(2);
		}
		numbytes = recvfrom(sockfd, buf, MAXBUFLEN, 0, NULL, NULL);
		if (numbytes >= TCP_size)
		{
			memcpy(&header, buf, TCP_size);
			if(header.code == ACK || header.code == DATA)
				break;		
		}			
	}
	printf("Connection Established\n");
	int data_len = MAXBUFLEN - TCP_size;
	recv_file = fopen(destinationFile, "wb");
	printf("Recieving File...\n");
	uint16_t code, seq, length;
	while(1)
	{
		numbytes = recvfrom(sockfd, buf, MAXBUFLEN, 0, NULL, NULL);
		if (numbytes < TCP_size)
			continue;
		memcpy(&header, buf, TCP_size);
		code = header.code;
		if (code == FIN)
			break;		
		if (code != DATA)
			continue;
		seq = header.seq_no;
		length = numbytes - TCP_size;
		if(present[seq] == 0)
		{
			present[seq] = 1;
			if (SEEK_CUR != seq * data_len)
				fseek(recv_file, seq * data_len, SEEK_SET);
			fwrite(buf + TCP_size, 1, length, recv_file);
			while(present[NFE])
				NFE++;
		}
		header.code = ACK;
		memcpy(buf, &header, TCP_size);
		if ((numbytes = sendto(sockfd, buf, TCP_size, 0, (struct sockaddr *)&their_addr, 
			their_addr_len)) == -1) 
		{
			perror("receiver: sendto");
			exit(2);
		}
		if(seq > LFA)
			LFA = seq;	
	}
		
	header.code = FIN_ACK;
	memcpy(buf, &header, TCP_size);
	printf("Connection closed by sender\n");
	for(i = 0; i < 5; i++)
	{
		if ((numbytes = sendto(sockfd, buf, TCP_size, 0, (struct sockaddr *)&their_addr, 
			their_addr_len)) == -1) 
		{
			perror("receiver: sendto");
			exit(2);
		}	
	}

	fclose(recv_file);
	close(sockfd);
}

int main(int argc, char** argv)
{
	if(argc != 3)
	{
		fprintf(stderr, "usage: %s UDP_port filename_to_write\n\n", argv[0]);
		exit(1);
	}
	reliablyReceive(argv[1], argv[2]);
}
