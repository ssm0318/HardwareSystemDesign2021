#include"fpga_api.h"
#include<stdio.h>
#include <stdlib.h>
#include <string.h>
#include<fcntl.h>
#include<unistd.h>
#include<sys/mman.h>
#include <sys/time.h>
#include<cstring>
#include<time.h>

#define min(x,y) (((x)<(y))?(x):(y))

double time_accum = 0.0;

FPGA::FPGA(off_t data_addr, off_t output_addr, int m_size, int v_size)
{
  m_size_ = m_size;
  v_size_ = v_size;

  m1_size_ = v_size * v_size;

  data_size_ = (m_size_+1)*v_size_; // fpga bram data size
  data_size_M = (2*v_size_)*v_size_*sizeof(float);

  fd_ = open("/dev/mem", O_RDWR);
  data_M = static_cast<float*>(mmap(NULL, data_size_M, PROT_READ|PROT_WRITE, MAP_SHARED, fd_, data_addr));
  data_ = new float[data_size_];	

  output_ = static_cast<unsigned int*>(mmap(NULL, sizeof(unsigned int), PROT_READ|PROT_WRITE, MAP_SHARED,fd_, output_addr));
  output_MV = new unsigned int[m_size_];
  
  ps_dram_weight = static_cast<float*>(mmap(NULL, m1_size_*sizeof(float), PROT_READ|PROT_WRITE, MAP_SHARED, fd_, 0x10000000));
  ps_dram_input = static_cast<float*>(mmap(NULL, m1_size_*sizeof(float), PROT_READ|PROT_WRITE, MAP_SHARED, fd_, 0x10002000));
  fpga_dma = static_cast<unsigned int*>(mmap(NULL, 16*sizeof(unsigned int), PROT_READ|PROT_WRITE, MAP_SHARED, fd_, 0x7E200000));
  fpga_ip = static_cast<unsigned int*>(mmap(NULL, sizeof(unsigned int), PROT_READ|PROT_WRITE, MAP_SHARED, fd_, 0x43C00000));

  num_block_call_ = 0;
}

FPGA::~FPGA()
{
  munmap(data_M, data_size_M);
  munmap(output_, sizeof(unsigned int));
  munmap(ps_dram_weight,  m1_size_*sizeof(float));
  munmap(ps_dram_input,  m1_size_*sizeof(float));
  munmap(fpga_dma, 16*sizeof(unsigned int));
  munmap(fpga_ip, sizeof(unsigned int));
  close(fd_);

  delete[] data_;
  printf("total hardware time cost: %f\n", time_accum/CLOCKS_PER_SEC);
}

float* FPGA::matrix(void)
{
  return data_ + v_size_;
}

float* FPGA::vector(void)
{
  return data_;
}

float* FPGA::matrix_M1(void)
{
  return data_M;
}

float* FPGA::matrix_M2(void)
{
  return data_M + m1_size_;
}

void FPGA::reset(void)
{
  num_block_call_ = 0;
}

int FPGA::num_block_call(void)
{
  return num_block_call_;
}

const float* FPGA::blockMV()
{
  num_block_call_ += 1;

  // cpu version
  float* vec = this->vector();
  float* mat = this->matrix();
  float* out  = reinterpret_cast<float*>(output_MV);  

  for(int i = 0; i < m_size_; ++i)
  {
    out[i] = 0;
    for(int j = 0; j < v_size_; ++j)
      out[i] += vec[j] * mat[v_size_*i + j];
  }

  for(int i = 0; i < m_size_; ++i)
    data_[i] = out[i];

  return data_;    
}

const float* __attribute__((optimize("O0"))) FPGA::blockMM()
{
  num_block_call_ += 1;

  // fpga version
  clock_t start = clock();
  *fpga_ip = 0x5555;
  while(*fpga_ip == 0x5555);
  clock_t end = clock();

  time_accum += (double)(end - start);

  return data_M;    
}

void FPGA::largeMV(const float* large_mat, const float* input, float* output, int num_input, int num_output)
{
  float* vec = this->vector();
  float* mat = this->matrix();

  // 0) Initialize output vector		
  for(int i = 0; i < num_output; ++i)
    output[i] = 0;

  for(int i = 0; i < num_output; i += m_size_)
  {
    for(int j = 0; j < num_input; j += v_size_)
    {			
      // 0) Initialize input vector
      int block_row = min(m_size_, num_output-i);
      int block_col = min(v_size_, num_input-j);

      // 1) Assign a vector
     	memset(vec, 0, v_size_);
			memcpy(vec, input + j, sizeof(float) * block_col);

      // 2) Assign a matrix
			for(int row = 0; row < m_size_; ++row)
			{
				memset(mat + (v_size_*row), 0, sizeof(float) * v_size_);
				if(row < block_row)
				{
					memcpy(mat + (v_size_*row), large_mat + (num_input*(i+row)) + j, sizeof(float) * block_col);
				}
			}

      // 3) Call a function `blockMV() to execute MV multiplication
      const float* ret = this->blockMV();

      // 4) Accumulate intermediate results
      for(int row = 0; row < block_row; ++row)
        output[i + row] += ret[row];
    } 
  }
}

void FPGA::largeMM(const float* weight_mat, const float* input_mat, float* output, int num_input, int num_output, int num_matrix2)
{
  float* m1 = this->matrix_M1();
  float* m2 = this->matrix_M2();

  memcpy(ps_dram_weight, weight_mat, m1_size_*sizeof(float));
  memcpy(ps_dram_input, input_mat, m1_size_*sizeof(float));

  // 0) Initialize output vector		
  for(int i = 0; i < num_output*num_matrix2; ++i)
    output[i] = 0;

  for(int i = 0; i < num_output; i += v_size_)
  {
    for(int j = 0; j < num_input; j += v_size_)
    {			
      for(int k = 0; k < num_matrix2; k += v_size_)
      {
        // 0) Initialize input vector
        int block_row = min(v_size_, num_output-i);
        int block_col_1 = min(v_size_, num_input-j);
        int block_col_2 = min(v_size_, num_matrix2-k);
        
        // 1) Assign a m1
        *(fpga_dma+6) = 0x10000000 + (num_input * i + j) * sizeof(float);
        *(fpga_dma+8) = 0xC0000000 + v_size_ * sizeof(float);
        *(fpga_dma+10) = sizeof(float) * block_col_1 * v_size_;
        while ((*(fpga_dma+1) & 0x00000002) == 0);

        *(fpga_dma+6) = 0x10000000 + ((num_input * i + j) * sizeof(float)) + (sizeof(float) * block_col_1 * v_size_);
        *(fpga_dma+8) = 0xC0000000 + (v_size_ * sizeof(float)) + (sizeof(float) * block_col_1 * v_size_);
        *(fpga_dma+10) = sizeof(float) * (v_size_ - block_col_1) * v_size_;
        while ((*(fpga_dma+1) & 0x00000002) == 0);

        if(!i && !j && !k) {
          printf("m1\n");
          printf("fpga_dma address %p\n", fpga_dma);
          printf("weight_mat address %p\n", weight_mat);
          printf("fpga_dma+6 x %X\n", *(fpga_dma+6));
          printf("fpga_dma+8 x %X\n", *(fpga_dma+8));
          printf("fpga_dma+10 d %d\n", *(fpga_dma+10));
          // printf("data_M f %f\n", *data_M);
          printf("data_ f %f\n", *data_);
          printf("weight_mat f %f\n", *weight_mat);
        }

        // 2) Assign a m2
        // *(fpga_dma+6) = (unsigned int) input_mat;
        *(fpga_dma+6) = 0x10000000 + (num_matrix2 * j + k) * sizeof(float);
        *(fpga_dma+8) = 0xC0000000 + v_size_ * sizeof(float);
        *(fpga_dma+10) = sizeof(float) * block_col_2 * v_size_;
        while ((*(fpga_dma+1) & 0x00000002) == 0);

        *(fpga_dma+6) = 0x10000000 + ((num_matrix2 * j + k) * sizeof(float)) + (sizeof(float) * block_col_2 * v_size_);
        *(fpga_dma+8) = 0xC0000000 + (v_size_ * sizeof(float)) + (sizeof(float) * block_col_2 * v_size_);
        *(fpga_dma+10) = sizeof(float) * (v_size_ - block_col_2) * v_size_;
        while ((*(fpga_dma+1) & 0x00000002) == 0);

        // 3) Call a function `blockMM() to execute Matrix matrix multiplication
        const float* ret = this->blockMM();

        if(!i && !j && k == 8) {
          printf("m2\n");
          printf("fpga_dma address %p\n", fpga_dma);
          printf("input_mat address %p\n", input_mat);
          printf("fpga_dma+6 x %X\n", *(fpga_dma+6));
          printf("fpga_dma+8 x %X\n", *(fpga_dma+8));
          printf("fpga_dma+10 d %d\n", *(fpga_dma+10));
          // printf("data_M f %f\n", *data_M);
          printf("data_ f %f\n", *(data_ + m1_size_));
          printf("input_mat f %f\n", *input_mat);
        }

        // 4) Accumulate intermediate results
        *(fpga_dma+6) = 0xC0000000;
        *(fpga_dma+8) = 0x10000000;
        *(fpga_dma+10) = sizeof(float) * v_size_ * v_size_;
        while ((*(fpga_dma+1) & 0x00000002) == 0);
        
        for(int n = 0; n<block_row; ++n)
        {
          for(int m = 0; m<block_col_2; ++m)
          {
            output[(i + n) + (k + m)*num_output] += ret[n*v_size_ + m];
          }
        }
      }
    } 
  }
}

void FPGA::convLowering(const std::vector<std::vector<std::vector<std::vector<float>>>>& cnn_weights,
    std::vector<std::vector<float>>& new_weights,
    const std::vector<std::vector<std::vector<float>>>& inputs,
    std::vector<std::vector<float>>& new_inputs) {
  /*
   * Arguments:
   *
   * conv_weights: [conv_channel, input_channel, conv_height, conv_width]
   * new_weights: [?, ?]
   * inputs: [input_channel, input_height, input_width]
   * new_inputs: [?, ?]
   *
   */

  int conv_channel = cnn_weights.size();
  int input_channel = cnn_weights[0].size();
  int conv_height = cnn_weights[0][0].size();
  int conv_width = cnn_weights[0][0][0].size();
  //int input_channel = cnn_weights.size();
  int input_height = inputs[0].size();
  int input_width = inputs[0][0].size();

  // IMPLEMENT THIS
  // For example,
  // new_weights[0][0] = cnn_weights[0][0][0][0];
  // new_inputs[0][0] = inputs[0][0][0];
  
  // new_weights
  for(int c = 0; c < conv_channel; c++) {
    for(int i = 0; i < input_channel; i++) {
      for(int h = 0; h < conv_height; h++) {
        for(int w = 0; w < conv_width; w++) {
          new_weights[c][(i*conv_height*conv_width) + (h*conv_width+w)] = cnn_weights[c][i][h][w];
        }
      }
    }
  }

  // new inputs
  for(int h = 0; h <= (input_height-conv_height); h++) {
    for(int w = 0; w <= (input_width-conv_width); w++) {
      for(int i = 0; i < input_channel; i++) {
        for(int y = 0; y < conv_height; y++) {
          for(int x = 0; x < conv_width; x++) {
            new_inputs[(i*conv_height*conv_width) + (y*conv_width+x)][h*(input_width-conv_width+1)+w] = inputs[i][h+y][w+x];
          }
        }
      }
    }
  }
}