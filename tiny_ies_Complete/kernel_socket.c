
#include "tinyos.h"
#include "kernel_streams.h"
#include "kernel_dev.h"
#include "kernel_cc.h"
#include "util.h"

file_ops unbound_fops = 
{
	.Open = NULL,
	.Read = NULL,
	.Write = NULL,
	.Close = close_socket
};

file_ops listener_fops = 
{
	.Open = NULL,
	.Read = NULL,
	.Write = NULL,
	.Close = close_listener_socket
};

file_ops peer_fops = 
{
	.Open = NULL,
	.Read = read_peer_socket,
	.Write = write_peer_socket,
	.Close = close_peer_socket
};

Fid_t sys_Socket(port_t port)
{
	if(port < NOPORT || port > MAX_PORT) return NOFILE;//reassure that port is within limits

	Fid_t fid;
	FCB* fcb;
	int rsv = FCB_reserve(1, &fid, &fcb);	//reserve an fcb to be bound to a port
	if (!rsv) return NOFILE;	//check for available fid

	SCB* socket = (SCB*) xmalloc(sizeof(SCB));	//make new socket(UNBOUND)
	socket = create_SCB(socket, port, fcb);
	fcb->streamobj = socket;
	fcb->streamfunc = &unbound_fops;

	return fid;
}

int sys_Listen(Fid_t sock)
{
	FCB* fcb = get_fcb(sock);
	//if this nonNULL-fcb doesn't have socket_ops, not a socket, return -1
 	if(fcb != NULL && fcb->streamfunc != &unbound_fops) return -1;

 	//if sys_Socket returned (NOFILE) or sock points to a NULL fcb
    if(sock == NOFILE || fcb == NULL) return -1;  

	//valid fid
	if (fcb->streamobj != NULL) 
	{
    	SCB* scb = fcb->streamobj;
    	//if nonNULL socket bounded to a port AND socket=UNBOUND(only they can be LISTENERs) and their bounded port mustn't have a LISTENER
		if(scb->port != NOPORT && (scb->socket_type == UNBOUND && PORT_MAP[scb->port] == NULL))
		{	
			//make UNBOUND -> LISTENER
			scb->socket_type = LISTENER;	

		   //initialize LISTENER socket
			LISTENER_SCB* lscb = &scb->listener_socket;
			lscb->reqs = COND_INIT;
			rlnode_init(& lscb->req_queue, NULL);
			fcb->streamfunc = &listener_fops;

			PORT_MAP[scb->port] = scb; //Transfer socket to its port map
			return 0;
		}
	}
	return -1;
}

Fid_t sys_Accept(Fid_t lsock)
{
	FCB* fcb = get_fcb(lsock);

	if((lsock == NOFILE || fcb == NULL) || (fcb != NULL && fcb->streamfunc != &listener_fops)) return NOFILE;	//return -1
	
	SCB* listener = fcb->streamobj;
	//reassure it is a listener
	if(listener == NULL || listener->socket_type != LISTENER) return NOFILE;

	if (fcb != NULL && fcb->streamobj != NULL){
		Fid_t new_fid = sys_Socket(NOPORT);////create new socket for the peerToPeer connection
		if (new_fid == NOFILE) return NOFILE;	//(valid) if FIDs of the process are EXHAUSTED then ERROR
		
		//if request queue is empty, there is no request, so wait until we get one
		while(is_rlist_empty(& listener->listener_socket.req_queue))
		{
			kernel_wait(&listener->listener_socket.reqs, SCHED_USER);

			//if listener has closed whilw sleeping then ERROR
			if(PORT_MAP[listener->port] == NULL) return NOFILE;
		}
		
		FCB* new_fcb = get_fcb(new_fid);
		SCB* conn_scb = new_fcb->streamobj;

		rlnode* request_node = rlist_pop_front(&listener->listener_socket.req_queue); //get the request(socket_req) from the queue 
		request* rqst = request_node->rqst;
		SCB* req_scb = rqst->scb;
		
		connect_peer2peer(req_scb, conn_scb); //connect two peer sockets
		rqst->accepted = 1; 
		kernel_signal(& rqst->request_cv);//wake up thread which requested since request accepted
		return new_fid;	//succed
	}
	return NOFILE;
}

int sys_Connect(Fid_t sock, port_t port, timeout_t timeout)
{
	if((port < NOPORT || port > MAX_PORT) || PORT_MAP[port] == NULL) return -1;

	SCB* listener = PORT_MAP[port];
	//reassure it is a listener
	if(listener == NULL || listener->socket_type != LISTENER) return -1;

	FCB* fcb = get_fcb(sock);

	if(fcb == NULL || fcb->streamobj == NULL) return -1;
	
	SCB* scb = fcb->streamobj;

	//socket must be UNBOUND to call create_request
	if (scb->socket_type == UNBOUND)
	{
		request* rqst = create_request(scb, listener); 			
		kernel_signal(& listener->listener_socket.reqs);//wake up listener
		
		scb->refcount++; //keep socket open during sleep
		
		kernel_timedwait(& rqst->request_cv, SCHED_USER, timeout);//timeout sleep, wake up when accepted
		scb->refcount--; 
		int success = rqst->accepted; 
		free(rqst);

		if(success) return 0;
			
	}	
	return -1;
}

int sys_ShutDown(Fid_t sock, shutdown_mode how)
{
	FCB* fcb = get_fcb(sock);
	if((sock == NOFILE || fcb == NULL) || (fcb != NULL && fcb->streamfunc != &peer_fops)) return -1;
	
	//get peer from sock , only peer sockets shutdown
	SCB* scb = fcb->streamobj;
	// Deoending on how ShutDown was called (argument) we close the end of pipe
    if(how == SHUTDOWN_READ){		//stop pipe's op. that peer reads
        pipe_reader_close(scb->peer_socket.pipe_recv);
		scb->peer_socket.pipe_recv = NULL;
        return 0;
    }
    else if(how == SHUTDOWN_WRITE){		//stop pipes's op. that peer writes
    	pipe_writer_close(scb->peer_socket.pipe_send);
		scb->peer_socket.pipe_send = NULL;
        return 0;
    }
    else{								//(BOTH) stop 2 opposite pipes' ops.
        pipe_reader_close(scb->peer_socket.pipe_recv);
		scb->peer_socket.pipe_recv = NULL;
		pipe_writer_close(scb->peer_socket.pipe_send);
		scb->peer_socket.pipe_send = NULL;
        return 0;
    }

    return -1;
}

int close_socket(void* scb){
	SCB* socket = scb; 
	if(PORT_MAP[socket->port] != NULL)
     PORT_MAP[socket->port] = NULL; /** Free this cell of PORT_MAP **/

    release_SCB(socket);
    return 0;
}
int close_listener_socket(void* scb){
	SCB* socket = scb;
	PORT_MAP[socket->port] = NULL; //make listener port NULL
	kernel_signal(& socket->listener_socket.reqs);//wake up listener
	close_socket(socket);
	return 0;
}

int close_peer_socket(void* scb){
	SCB* peer_scb = scb;
	if (peer_scb->refcount == 0)
	{
		if(peer_scb->peer_socket.pipe_recv != NULL)
			pipe_reader_close(peer_scb->peer_socket.pipe_recv);

		if(peer_scb->peer_socket.pipe_send != NULL)
			pipe_writer_close(peer_scb->peer_socket.pipe_send);
		close_socket(peer_scb);
		return 0;
	}
	else
		return -1;
}

int read_peer_socket(void* scb, char* buf, unsigned int size){
	SCB* peer_scb = scb;

	PIPECB* cur_pipe = peer_scb->peer_socket.pipe_recv;
	if (cur_pipe==NULL)
	{
		return -1;
	}else{
		return pipe_read(cur_pipe, buf, size);
	}
}

int write_peer_socket(void* scb, const char* buf, unsigned int size){
	SCB* peer_scb = scb;
	PIPECB* cur_pipe = peer_scb->peer_socket.pipe_send;
	if (cur_pipe==NULL)
	{
		return -1;
	}else{
		return pipe_write(cur_pipe, buf, size);
	}
}

SCB* create_SCB(SCB* new_scb, port_t port, FCB* fcb)
{
	new_scb->refcount = 0;
	new_scb->port = port;
	new_scb->fcb = fcb;
	new_scb->socket_type = UNBOUND;
	return new_scb;
}

void release_SCB(SCB* scb){
	free(scb);
}


void connect_peer2peer(SCB* scb1, SCB* scb2)
{
	//create and init 2 pipecbs
	PIPECB* pipe1 = acquire_PIPECB();	
	pipe1->reader = (FCB*)1;
	pipe1->writer = (FCB*)1;
	pipe1->hasData = COND_INIT;
    pipe1->hasSpace = COND_INIT;
    pipe1->head = 0;
    pipe1->numOfElements = 0;

	PIPECB* pipe2 = acquire_PIPECB();	
	pipe2->reader = (FCB*)1;
    pipe2->writer = (FCB*)1;
	pipe2->hasData = COND_INIT;
    pipe2->hasSpace = COND_INIT;
	pipe2->head = 0;
    pipe2->numOfElements = 0;

	scb2->socket_type = PEER;
	//initialize peer socket 1
	PEER_SCB* pscb2 = &scb2->peer_socket;
	pscb2->pipe_recv = pipe2;	//pipe receive
	pscb2->pipe_send = pipe1;	//pipe send
	scb2->fcb->streamfunc = &peer_fops;

	scb1->socket_type = PEER;
	//initialize peer socket 2
	PEER_SCB* pscb1 = &scb1->peer_socket;
	pscb1->pipe_recv = pipe1;
	pscb1->pipe_send = pipe2;
	scb1->fcb->streamfunc = &peer_fops;
}

request* create_request(SCB* scb, SCB* listener)
{
	request* rqst = (request*) xmalloc(sizeof(request));
	rqst->scb = scb;
	rqst->request_cv = COND_INIT;
	rqst->accepted = 0;
	rlnode_init(& rqst->request_node, rqst);
	rlist_push_back(& listener->listener_socket.req_queue, &rqst->request_node);
	return rqst;
}