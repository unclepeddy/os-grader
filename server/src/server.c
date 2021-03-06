#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "server.h"
#include "macros.h"

/*
** Parses the header of the request and returns an array of tokens
*/
int parse_arguments(char **args, char *line) {
  int i = 0;
  args[i] = strtok(line, ARG_DELIM);
  while ((args[++i] = strtok(NULL, ARG_DELIM)) != NULL);
  return i - 1;
}

/*
** Fills newfile buffer with new filename of format <user>_<module_num>_XXXXX.c
** Returns a pointer to newfile, where XXXXX is a "random" number.
*/
char *generate_filename(char *newfile, char *user, char *module_num) {
  int k=0;
  
  // Add username
  int i;
  for (i=0;i<strlen(user);) { 
    newfile[k++] = user[i++];
  }

  newfile[k++] = '_';

  // Add module number
  for (i=0; i<strlen(module_num);) {
    newfile[k++] = module_num[i++];
  }

  newfile[k++] = '_';

  // Seed random generator and produce "random" 5 digit number
  time_t t;
  srand((unsigned) time(&t)); 
  int r = rand() % 10000;

  // Add random number to filename
  sprintf(newfile + k, "%05d.c", r);

  return newfile;
}

/*
** Sets up listen queue socket and returns its file descriptor
** If could not bind socket, returns -1
*/
int set_up_server() {
  
  // Create socket to listen for incoming connections
  int listen_queue_socket = socket(AF_INET, SOCK_STREAM, 0);
  if (listen_queue_socket < 0) {
    printf("Cannot create socket\n");
    close(listen_queue_socket);
    return 1;
  }

  struct sockaddr_in server_address;
  server_address.sin_family = AF_INET;
  server_address.sin_addr.s_addr = htonl(INADDR_ANY);
  server_address.sin_port = htons(PORT);

  // Bind the socket to server_address
  if (bind(listen_queue_socket, (struct sockaddr *)&server_address,
           sizeof(server_address)) < 0) {
    printf("Cannot bind socket\n");
    close(listen_queue_socket);
    return -1;
  }
  if (DEBUG) printf("Created and bound socket\n\n");
  return listen_queue_socket;
}

/*
** Blocks and waits for a client to make a request
** Upon receipt, moves the connection to a new socket and returns its fd
*/
int listen_for_requests(int listen_queue_socket) {
  int connection_socket;
  socklen_t client_address_len;
  struct sockaddr_in client_address; 
  
  // Listen for new connections
  if (listen(listen_queue_socket, MAX_WAITING_CONNECTIONS) < 0) {
    printf("Could not listen on port %d\n", PORT);
    return -1;
  }

  // Accept new connections, move them to connection_socket
  client_address_len = sizeof(client_address);
  connection_socket =
      accept(listen_queue_socket, (struct sockaddr *)&client_address,
             &client_address_len);

  return connection_socket;
}

/* 
** Actually does the job of receiving the request data from the client
** Returns 0 for no errors and 1 if any errors occur.
*/
int receive_request(Request *request) {

  int connection_socket = request->socket_fd;

  // Make sure connection socket descriptor is valid
  if (connection_socket < 0) {
    return 1;
  }

  // Receive the header_tokens and write the file
  char receive_buffer[TCP_PACKET_SIZE];
  if (recv(connection_socket, receive_buffer, sizeof(receive_buffer), 0)) {
    
    char *header_tokens[TCP_PACKET_SIZE];
    char new_filename[MAX_FILENAME_LEN];
    int bytes_remaining, bytes_received;

    // Make sure the first packet is the header data
    if (!strncmp(receive_buffer, HEADER_PREFIX, strlen(HEADER_PREFIX))) {

      // Parse the header and capture data
      parse_arguments(header_tokens, receive_buffer);
      strcpy(request->user, header_tokens[1]);
      strcpy(request->module_num, header_tokens[2]);
      bytes_remaining = atoi(header_tokens[3]);
      strcpy(request->filename, generate_filename(&new_filename, request->user, request->module_num));
    
    } else {
      // Print error message and exit
      printf("Failed to receive header - first packet content: [%s]\n",
             receive_buffer);
      close(connection_socket);
      return 1;
    }

    // Compute path of file to write
    char filepath[MAX_FILENAME_LEN];
    strcpy(&filepath, TEMP);
    strcpy(&filepath[strlen(TEMP)], "/");
    strcpy(&filepath[strlen(TEMP) + 1], request->filename);

    // Create TEMP if it does not exist
    if (access(TEMP, F_OK) == -1) system("mkdir "TEMP);

    if (VERBOSE) printf("Retreived and parsed header: <%s><%s><%d>\n", 
      request->user, request->module_num, bytes_remaining);

    // Send header received ack
    send(connection_socket, HDR_AOK, strlen(HDR_AOK), 0);

    // Open file to write to and
    FILE *file_to_write = open(filepath, O_WRONLY | O_TRUNC | O_CREAT, 00664);

    if (VERBOSE) printf("Opened file to write to\n");

    int total_received=0;
    // Listen until file is received or no more packets are being sent
    while ((bytes_remaining > 0) && 
        ((bytes_received = recv(connection_socket, receive_buffer, TCP_PACKET_SIZE, 0)) > 0)) {

      // Write the received packet of data to the file
      write(file_to_write, receive_buffer, bytes_received);
      total_received += bytes_received;
      bytes_remaining -= bytes_received;
    }

    // Make sure we received the number of bytes we expected
    if (!bytes_remaining) {
      if (VERBOSE) printf("Succesfully retrieved file\n\n");
    } else {
      printf("Warning - Expected %d bytes, received %d bytes\n\n", atoi(header_tokens[3]), total_received);
    }

    close(file_to_write);

    // Send ack to client
    send(connection_socket, REQ_AOK, strlen(REQ_AOK), 0);

  } else {
    printf("Client dropped connection\n\n");
  }
}

/*
** Assumes socket_fd is a valid connection
** Sends message on the socket connection
*/
int send_message(int socket_fd, char *message) {
  // send ack to client
  int status = send(socket_fd, message, strlen(message), 0);
}
  
/*
** Assumes socket_fd is a valid connection
** Sends the solution for specifed module over the connection
*/
int send_solution(int socket_fd, char *module_num) {

  // Compute solution file path
  char sol_file[MAX_FILENAME_LEN];
  sprintf(sol_file, "%s/%s/%s_%s%s", MODULES, module_num, SOLFILE_PREFIX,
      module_num, SOLFILE_SUFFIX); 

  // Open solution file
  FILE *file;
  if ((file = fopen(sol_file , "r")) < 1) {
    printf("No solution file %s\n", sol_file);
    return -1;
  }

  // Compute file size
  struct stat sol_stat;
  stat(sol_file, &sol_stat);
  int bytes_remaining = sol_stat.st_size;

  // Send begining of solution ack
  send(socket_fd, BEG_SOL, strlen(BEG_SOL), 0);
  if (VERBOSE) printf("Sent BEG_SOL ack\n");

  // Send file header
  char buffer[TCP_PACKET_SIZE];
  memset(buffer, 0, TCP_PACKET_SIZE);
  sprintf(buffer, "%s%s%s%s%d%s", HEADER_PREFIX, ARG_DELIM, module_num,
      ARG_DELIM, bytes_remaining, ARG_DELIM);
  send(socket_fd, buffer, strlen(buffer), 0);
  if (VERBOSE) printf("Sent file header [%s]\n", buffer);

  // Wait for header received ack
  memset(buffer, 0, TCP_PACKET_SIZE);
  recv(socket_fd, buffer, strlen(HDR_AOK), 0);
  if (strcmp(buffer, HDR_AOK)) return 1;
  if (VERBOSE) printf("Received HDR_AOK\n");

  // Send file content
  int bytes_read;
  while (bytes_remaining > 0) {
    bytes_read = fread(buffer, sizeof(buffer[0]), TCP_PACKET_SIZE, file); 
    send(socket_fd, buffer, bytes_read, 0);
    bytes_remaining -= bytes_read;
  }
  if (VERBOSE) printf("Sent file content\n");

  fclose(file);

  // Wait for ack from client
  memset(buffer, 0, TCP_PACKET_SIZE);
  recv(socket_fd, buffer, TCP_PACKET_SIZE, 0);
  if (strcmp(buffer, FIL_AOK)) {
    printf("Received bad ack (%s)\n", buffer);
    return 1;
  }

  if (VERBOSE) printf("Received FIL_AOK ack\n");
  return 0;
}

/*
** Sends the content of the file over the socket.
*/
int send_file(int socket_fd, char *filepath) {
  FILE *r_file;
  if ((r_file = fopen(filepath ,"r")) < 1) {
    printf("No error log file %s\n", filepath);
    return -1;
  }

  // Compute file size
  struct stat log_stat;
  stat(filepath, &log_stat);
  int bytes_remaining = log_stat.st_size;
  
  // If empty file, return
  if (!bytes_remaining) {
    fclose(r_file);
    return -1;
  }

  // Send ack that a file is being streamed over
  send(socket_fd, BEG_FIL, strlen(BEG_FIL), 0);

  // Send file content
  char buffer[TCP_PACKET_SIZE];
  int bytes_read;
  while (bytes_remaining > 0) {
    bytes_read = fread(buffer, sizeof(buffer[0]), TCP_PACKET_SIZE, r_file); 
    send(socket_fd, buffer, bytes_read, 0);
    bytes_remaining -= bytes_read;
  }

  fclose(r_file);
  return 0;
}

/*
** Closes the socket connection and does any necassary clean-up.
*/
int close_connection(int socket_fd) {
  /* Do any other clean up*/
  return close(socket_fd);
}

int example_server_main() { 

  // Set up initial socket for listen queue
  int listen_queue_socket = set_up_server();

  while (1) {
    
    // Block and listen for connections - upon receipt, move connection to new socket
    int connection_socket = listen_for_requests(listen_queue_socket);

    Request request;
    request.socket_fd = connection_socket;

    // Let the handler receive request data
    // TODO: Move this to a new thread
    receive_request(&request);
  }
}
