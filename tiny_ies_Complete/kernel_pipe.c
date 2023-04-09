#include "tinyos.h"
#include "kernel_dev.h"
#include "kernel_streams.h"
#include "kernel_cc.h"
#include "util.h"


int buf_empty = 1;

///////////////////////////////////////READER/////////////////////////////////////////////

int read_first_flag=0;
int write_flag=0;

int pipe_read(void* ctrl_block, char *buf, unsigned int size) {

    read_first_flag=1;

    PIPECB *pipe_ctrl = (PIPECB*) ctrl_block;


    int count =  0; // num of read chars

    if(pipe_ctrl->reader== NULL) {
        read_first_flag=0;
        return -1;
    }

    if (write_flag == 1)
        Cond_Broadcast(&(pipe_ctrl->hasSpace)) ;

    if ((pipe_ctrl->numOfElements == 0) && (pipe_ctrl->writer !=NULL) && (write_flag == 0)) {
        //if NO DATA and WRITE IS OPEN then READ should SLEEP
        Cond_Broadcast(&(pipe_ctrl->hasSpace)) ;
        kernel_wait( &pipe_ctrl->hasData,SCHED_PIPE);
    }

    if ((pipe_ctrl->numOfElements != 0 ) || (write_flag != 0)) {//if the pipe is not empty AND the writer is not enabled

        while(count<size) {
            buf[count] = pipe_ctrl->p_buf[pipe_ctrl->head] ; // Transfer data from the pipe to the external buffer
            count++;
            // Calculation of new head . numberofElements
            pipe_ctrl->head = (pipe_ctrl->head + 1 ) % MAX_PIPE  ; //Reassure that the ne head is within array limits

            if (pipe_ctrl->numOfElements >= 0 )
                pipe_ctrl->numOfElements-- ;

            if ( pipe_ctrl->numOfElements == 0 ) {
                if (write_flag == 1) {
                    Cond_Broadcast(&(pipe_ctrl->hasSpace)) ;
                    kernel_wait(&pipe_ctrl->hasData,SCHED_PIPE);
                }
                else  {
                    break	;
                }

            }
        }

    }

    read_first_flag=0;
    return count; // Returns the number of bytes/chars it read
}

int pipe_write(void* ctrl_block, const char* buf, unsigned int size) { 

    PIPECB *pipe_ctrl = (PIPECB*) ctrl_block;


    int count =  0; // Number of written chars

    if (pipe_ctrl->writer == NULL) {

        return -1;
    }
    if (pipe_ctrl->reader == NULL) {// Read end is closed, so write becomes unusable

        return -1;
    }

    while(count<size) {
        int write_position =  (pipe_ctrl->head + pipe_ctrl->numOfElements) % MAX_PIPE ;
        pipe_ctrl->p_buf[write_position] = buf[count] ; // Transfer of data from the external buffer to the pipe
        pipe_ctrl->numOfElements++ ;
        count++;

        if ((pipe_ctrl->numOfElements == MAX_PIPE) && (pipe_ctrl->reader!=NULL) && (count != size) ) {
            Cond_Broadcast (&(pipe_ctrl->hasData)) ; // Data to be read available so wake up read
            write_flag = 1 ;
            kernel_wait(&pipe_ctrl->hasSpace,SCHED_PIPE);
        }
    }
    write_flag = 0 ;
    if (read_first_flag)
        Cond_Broadcast (&(pipe_ctrl->hasData)); // Data to be read available so wake up read

    return count; // Returns the number of bytes/chars it wrote
}

int pipe_reader_close(void* ctrl_block) {

    PIPECB *pipe_ctrl = (PIPECB*) ctrl_block;
    pipe_ctrl->reader = NULL ;
    Cond_Broadcast (&(pipe_ctrl->hasSpace)) ;
    if (pipe_ctrl->writer== NULL )
        free (pipe_ctrl) ;
    return 0;
}

int pipe_writer_close(void* ctrl_block) {

    PIPECB *pipe_ctrl = (PIPECB*) ctrl_block;
    pipe_ctrl->writer = NULL ;
    if (pipe_ctrl->reader== NULL )
        free (pipe_ctrl) ;
    Cond_Broadcast (&(pipe_ctrl->hasData)) ;
    return 0;
}

file_ops pipeReader_fops = {
        .Open = NULL,
        .Read = pipe_read,
        .Write = NULL,
        .Close = pipe_reader_close
};


file_ops pipeWrite_fops = {
        .Open = NULL,
        .Read = NULL,
        .Write = pipe_write,
        .Close = pipe_writer_close
};

int sys_Pipe(pipe_t* pipe) {

    FCB* fcb_reader;
    FCB* fcb_writer;


    if((!FCB_reserve(1, &pipe->read, &fcb_reader)) | (!FCB_reserve(1, &pipe->write, &fcb_writer))) {// If the FIDs are EXHAUSTED

        return -1;
    }
    else {
        // Initialize a pipe control block and link it to streamobj (of both fids)
        PIPECB* newPipe = (PIPECB*) xmalloc(sizeof(PIPECB));

        fcb_reader->streamobj = newPipe ;
        fcb_writer->streamobj = newPipe ;

        // Initialize a pipeWrite_fops and pipeRead_fops and link them to the respective streamfuncs

        fcb_reader->streamfunc = &pipeReader_fops ;
        fcb_writer->streamfunc = &pipeWrite_fops ;

        newPipe->reader = fcb_reader;
        newPipe->writer = fcb_writer ;

        newPipe->hasData = COND_INIT;
        newPipe->hasSpace = COND_INIT;


        newPipe->head = 0;
        newPipe->numOfElements = 0 ;


        return 0 ;

    }


}
PIPECB* acquire_PIPECB()
{
    PIPECB* new_pipecb = xmalloc(sizeof(PIPECB));
    return new_pipecb;
}
void release_PIPECB(PIPECB* pipecb)
{
    free(pipecb->p_buf);
    free(pipecb);
}
