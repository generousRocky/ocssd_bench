#include <pthread.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <liblightnvm.h>
#include <string.h>
#include <time.h>
#include <errno.h>

#define NVM_DEV_PATH "/dev/nvme0n1"

#define NR_PUNITS 128
#define NR_BLOCKS 1020

#define NR_W_THREADS 1
#define NR_R_THREADS 1

#define NR_BLKS_IN_VBLK 128
#define NR_VBLKS 1020 * (NR_PUNITS / NR_BLKS_IN_VBLK)
size_t NBYTES_VBLK = (size_t)NR_BLKS_IN_VBLK*1024*1024*16;


//#define NBYTES_TO_WRITE  10737418240 // 10GB
#define NBYTES_TO_WRITE  21474836480 // 20GB
#define NBYTES_TO_READ  21474836480 // 20GB

enum{
	FREE,
	RESERVED,
	BAD,
};

static char nvm_dev_path[] = NVM_DEV_PATH;
static struct nvm_dev *dev;
static const struct nvm_geo *geo;
//
struct nvm_buf_set *bufs = NULL;

struct nvm_addr punits_[NR_PUNITS];
struct nvm_vblk* vblks_[NR_VBLKS];
int vblks_state[NR_VBLKS];
size_t curs=0;

size_t NBYTES_IO = (size_t)(NBYTES_TO_WRITE/NR_W_THREADS); // per a thread

//function level profiling
#define BILLION     (1000000001ULL)
#define calclock(timevalue, total_time, total_count, delay_time) do { \
  unsigned long long timedelay, temp, temp_n; \
  struct timespec *myclock = (struct timespec*)timevalue; \
  if(myclock[1].tv_nsec >= myclock[0].tv_nsec){ \
    temp = myclock[1].tv_sec - myclock[0].tv_sec; \
    temp_n = myclock[1].tv_nsec - myclock[0].tv_nsec; \
    timedelay = BILLION * temp + temp_n; \
  } else { \
    temp = myclock[1].tv_sec - myclock[0].tv_sec - 1; \
    temp_n = BILLION + myclock[1].tv_nsec - myclock[0].tv_nsec; \
    timedelay = BILLION * temp + temp_n; \
  } \
  *delay_time = timedelay; \
  __sync_fetch_and_add(total_time, timedelay); \
  __sync_fetch_and_add(total_count, 1); \
} while(0)

//Global function time/count variables
unsigned long long total_time, total_count;

pthread_mutex_t  mutex = PTHREAD_MUTEX_INITIALIZER; // 쓰레드 초기화 



void free_all(){
	for(size_t i=0; i<NR_VBLKS; i++){
		nvm_vblk_free(vblks_[i]);
	}
  return;
}

struct nvm_vblk* get_vblk(void){

	pthread_mutex_lock(&mutex);

  for (size_t i = 0; i < NR_VBLKS; i++) {
    
		curs++;
	  curs = curs % NR_VBLKS;

    switch (vblks_state[curs]) {
    case FREE:
      if (nvm_vblk_erase(vblks_[curs]) < 0) {
        vblks_state[curs] = BAD;
        break;
      }

      vblks_state[curs] = RESERVED;
			printf("returning vblk_idx: %zu\n", curs);
			pthread_mutex_unlock(&mutex);
			return vblks_[curs];

    case RESERVED:
    case BAD:
      break;
    }
  }

	printf("No available vblk\n");
	pthread_mutex_unlock(&mutex);
  return NULL;
}

void nvm_init(){

  size_t nbytes=0;

	/* open nvme device */
	dev = nvm_dev_open(nvm_dev_path);
	if (!dev) {
		perror("nvm_dev_open");
		return exit(-1);
	}

	geo = nvm_dev_get_geo(dev);

	// Construct punit addrs
	for (size_t i = 0; i < 128; ++i){
		struct nvm_addr addr;

		addr.ppa = 0;
		addr.g.ch = i % geo->nchannels;
		addr.g.lun = (i / geo->nchannels) % geo->nluns;

		punits_[i] = addr;
	}

  // Initialize and allocate vblks with defaults (Freene)
  
	size_t vblk_idx = 0;
	size_t nr_iterate_to_read = 0; // up to NBYTES_TO_READ / NBYTES_VBLK;
	printf("nr_iterate_to_read: %zu\n", nr_iterate_to_read);

	for(size_t blk_idx =0; blk_idx< NR_BLOCKS; blk_idx++){
		printf("blk_idx: %zu\n", blk_idx);
		for(size_t i=0; i<128/NR_BLKS_IN_VBLK; i++){
			struct nvm_vblk *blk;
			struct nvm_addr addrs[NR_BLKS_IN_VBLK];
			for(size_t j=0; j<NR_BLKS_IN_VBLK; j++){
				addrs[j] = punits_[NR_BLKS_IN_VBLK*i+j];
				addrs[j].g.blk = blk_idx;
			}

			printf("vblk_idx: %zu\n", vblk_idx);
			blk = nvm_vblk_alloc(dev, addrs, NR_BLKS_IN_VBLK);
			if (!blk) {
				perror("FAILED: nvm_vblk_alloc_line");
				exit(-1);
			}

			if(vblk_idx < NR_VBLKS){ // vblks for write;
				vblks_[vblk_idx] = blk; vblk_idx++;
				vblks_state[vblk_idx] = FREE;
			}
			else{
				if(nr_iterate_to_read < NBYTES_TO_READ/NBYTES_VBLK){
					if (nvm_vblk_erase(blk) < 0) {
						vblks_state[curs] = BAD;
					}
					else{
						// 쓰기
						// 버퍼 할당, 생각

						vblks_state[curs] = RESERVED;
						nr_iterate_to_read++;	
					}
				}
				
				vblks_[vblk_idx] = blk; vblk_idx++;
			}
		}
	}

	//////
	
	printf("Allocating nvm_buf_set\n");
	bufs = nvm_buf_set_alloc(dev, NBYTES_VBLK, 0);
	if (!bufs) {
		printf("FAILED: Allocating nvm_buf_set\n");
		exit(-1);
	}

	printf("nvm_buf_set_fill\n");
	nvm_buf_set_fill(bufs);

}

void *t_writer(void *data)
{
	size_t* size = (size_t*)data;
	printf("size: %zu\n", *size);
  
	printf("NBYTES_VBLK: %zu\n", NBYTES_VBLK);
	int nr_iterate = ((*size)/NBYTES_VBLK);
	
	if(*size%NBYTES_VBLK > 0){
		nr_iterate++;
	}

	printf("nr_iterate: %d\n", nr_iterate);
  struct nvm_vblk* vblk; 
  
	for(int i=0; i< nr_iterate; i++){
    
    printf("Allocating vblk\n");
    vblk = get_vblk();
    if(vblk == NULL){
      printf("FAILED: Allocating vblk\n");
      goto out;
    }
		
		printf("nvm_vblk_write\n");
    if (nvm_vblk_write(vblk, bufs->write, NBYTES_VBLK) < 0) {
      printf("FAILED: nvm_vblk_write\n");
      goto out;
    }
	}
	
  printf("nvm_vblk_write: done\n");
  return 0;

out:
  nvm_buf_set_free(bufs);
  free_all();
  exit(-1);
}

void *t_reader(void *data)
{
	int* size = (int*)data;
	/*
	if (nvm_vblk_read(vblk, bufs->read, NBYTES_VBLK) < 0) {
		printf("FAILED: nvm_vblk_read\n");
		goto out;
	}
	*/

	// init에서 써 준 만큼 vblk쓰기
	// thread 여러 개 일때 생각 해 보자. 
}

void *io_manager(void *data){

  struct timespec local_time[2];
  clock_gettime(CLOCK_MONOTONIC, &local_time[0]);
  unsigned long long delay_time;
  // 프로파일링 시작
	
	pthread_t w_thread[NR_W_THREADS];
	pthread_t r_thread[NR_R_THREADS];

	size_t args_w_size[NR_W_THREADS];
	size_t args_r_size[NR_R_THREADS];

	int thr_id;
	int status;
	int rw = *((int*)data);

	if(rw = 1){ // writer
		for(int i=0; i<NR_W_THREADS; i++){
			args_w_size[i] = NBYTES_IO;
			thr_id = pthread_create(&w_thread[i], NULL, t_writer, &args_w_size[i]);
			if (thr_id < 0){
				perror("thread create error : ");
				exit(0);
			}
		}	
	}
	else{ // reader
		for(int i=0; i<NR_R_THREADS; i++){
			args_r_size[i] = NBYTES_IO;
			thr_id = pthread_create(&r_thread[i], NULL, t_reader, &args_r_size[i]);
			if (thr_id < 0){
				perror("thread create error : ");
				exit(0);
			}
		}	
	}

	if(rw = 1){ // writer
		for(int i=0; i<NR_W_THREADS; i++){
			pthread_join(w_thread[i], (void **)&status);
		}	
	}
	else{ // reader
		for(int i=0; i<NR_R_THREADS; i++){
			pthread_join(r_thread[i], (void **)&status);
		}	
	}

	// 프로파일링 끝
  clock_gettime(CLOCK_MONOTONIC, &local_time[1]);
  calclock(local_time, &total_time, &total_count, &delay_time );

  printf("[WRITE], elapsed time: %llu,  total_time: %llu, total_count: %llu\n", delay_time, total_time, total_count);
}


int main()
{
	pthread_t m_thread[2];

	int thr_id;
	int status;

	int flag_writer=0;
	int flag_reader=1;

	nvm_init();

	thr_id = pthread_create(&m_thread[0], NULL, io_manager, &flag_writer);
	if (thr_id < 0){
		perror("thread create error : ");
		exit(0);
	}
#if 0

	thr_id = pthread_create(&m_thread[1], NULL, io_manager, &flag_reader);
	if (thr_id < 0){
		perror("thread create error : ");
		exit(0);
	}

	pthread_join(m_thread[1], (void **)&status);
#endif 
  pthread_join(m_thread[0], (void **)&status);

	// free
	free_all();
  nvm_buf_set_free(bufs);
	
	/* close device */
	nvm_dev_close(dev);
	
	return 0;
}
