#include "filterIir.h"

#define FILTER_LENGTH_DEFAULT (201)
#define FILTER_LENGTH_MAX (511)
#define FILE_CHUNK (100)

//iirConfig lpfL, lpfR, hpfL, hpfR;
iirChannel iirL, iirR;

int IIRcoeffsL_L[COEFFS_PER_BIQUAD*IIR_ORDER_MAX/2] = {0};
long IIRdelayBufferL_L[IIR_DELAY_BUF_SIZE] = {0};

int IIRcoeffsR_L[COEFFS_PER_BIQUAD*IIR_ORDER_MAX/2] = {0};
long IIRdelayBufferR_L[IIR_DELAY_BUF_SIZE] = {0};

int IIRcoeffsL_H[COEFFS_PER_BIQUAD*IIR_ORDER_MAX/2] = {0};
long IIRdelayBufferL_H[IIR_DELAY_BUF_SIZE] = {0};

int IIRcoeffsR_H[COEFFS_PER_BIQUAD*IIR_ORDER_MAX/2] = {0};
long IIRdelayBufferR_H[IIR_DELAY_BUF_SIZE] = {0};

void loadfilterIIR(char* ftype, char *fresponse, char *fpass, int Hz, int* target, int order)
{
    File          fileHandle;
    //int newCoeffs[FILTER_LENGTH_MAX] = {0};
    
    long shortHz = Hz;
    shortHz -= 10; //lowest cutoff frequency = 10Hz
    int fileBin = shortHz / FILE_CHUNK; //alias to 1000Hz files.
    shortHz %= FILE_CHUNK; //alias to 1000Hz bins.
    shortHz *= order; //201 tap long filters;
    shortHz *= 2; // 2 bytes per tap;
    char fileName[32];
    sprintf(fileName, "%s/%d/%s/%d.flt",ftype,fresponse,order,fpass,fileBin); //default file name

   #ifdef AUDIO_INTERRUPTION
   AudioC.detachIntr(); //turning off the audio fixes audio / sd card collision
   #endif
   fileHandle = SD.open(fileName, FILE_READ);
    if(fileHandle)
    {

      fileHandle.seek(shortHz);
     fileHandle.read(target, order); //load the data
     fileHandle.close();
    
      for(int i = 0; i < order; i++) //fix endian-ness of dataset.
     {
       int temp = target[i];
       target[i] = ((temp & 0x00FF)<<8) + ((temp & 0xFF00)>>8);
     }          
    }

   #ifdef AUDIO_INTERRUPTION
   bool status = AudioC.Audio(TRUE);
   AudioC.setSamplingRate(SAMPLING_RATE_44_KHZ);
   if (status == 0)
   {
     AudioC.attachIntr(dmaIsr);
   }
   #endif
}

iirConfig initIIR(int *src, int *dst)
{
  iirConfig config;
  config.src = src;
  config.dst = dst;
  config.coeffs = 0;
  config.delayBuf = 0;
  config.order = 0;
  config.enabled = 0;
  return config;
  
}
iirConfig initIIR()
{
  return(initIIR(0,0));
}

void processIIR(iirConfig &config)
{
  if(config.enabled)
  {
    filter_iirArbitraryOrder(I2S_DMA_BUF_LEN, config.src, config.dst, config.coeffs, config.delayBuf, config.order);  
  }
  
}
iirChannel newIIRChannel()
{
    iirChannel newChannel;
    //initialize 4 IIR blocks to point to the inputs.
    newChannel.lpf = initIIR();
    newChannel.hpf = initIIR();
    newChannel.mode = 0;
    return newChannel;
}

void configureIIRChannel(iirChannel &channel, int mode, int* bufferin, int* bufferout, int* bufferint)
//the HPF destination buffer should always contain the final result of all filter operations.
{
  if(mode == LPF)
  {
    //low pass filter, configured such that the lpf directly goes 
    //in->[lpf]->out
    channel.lpf.src = bufferin;
    channel.lpf.dst = bufferout;
    
    channel.hpf.src = bufferin;
    channel.hpf.dst = bufferout;
    
    channel.lpf.enabled = 1;
    channel.hpf.enabled = 0;
  }
  else if(mode == HPF)
  {
    //high pass filter, configured such that the hpf directly goes 
    //in->[hpf]->out
    channel.lpf.src = bufferin;
    channel.lpf.dst = bufferout;
    
    channel.hpf.src = bufferin;
    channel.hpf.dst = bufferout;
    
    channel.lpf.enabled = 0;
    channel.hpf.enabled = 1;    
  }
  else if(mode == BPF)
  {
    //band pass filter, configured such that the filters are cascaded. 
    //in->[lpf]->intermediate->[hpf]->out
    channel.lpf.src = bufferin;
    channel.lpf.dst = bufferint;
    
    channel.hpf.src = bufferint;
    channel.hpf.dst = bufferout;
    
    channel.lpf.enabled = 1;
    channel.hpf.enabled = 1;
  }
  else if(mode == BSF)
  {
    //band stop filter, configured such that the filters are paralleled. 
    //in->[lpf]->intermediate, in->[hpf]->out, (out + intermediate) / 2 -> out 
    //(handled by the IIRProcessChannel function)
    channel.lpf.src = bufferin;
    channel.lpf.dst = bufferint;
    
    channel.hpf.src = bufferin;
    channel.hpf.dst = bufferout;
    
    channel.lpf.enabled = 1;
    channel.hpf.enabled = 1;
  }
  else //no filters enabled, point the output to the input, so that the fir filter routine copies the data out.
  {
    channel.lpf.src = bufferout;
    channel.lpf.dst = bufferout;
    
    channel.hpf.src = bufferout;
    channel.hpf.dst = bufferout;
    
    channel.lpf.enabled = 0;
    channel.hpf.enabled = 0;

  }
}

void deconfigureIIRChannel(iirChannel)
{
  
}

void IIRProcessChannel(iirChannel &channel)
{
  if (channel.mode == LPF) //low pass
  {
    processIIR(channel.lpf);
  }
  else if (channel.mode == HPF) //high pass
  {
    processIIR(channel.hpf);
  }
  else if (channel.mode == BPF) //band pass
  {
    processIIR(channel.lpf);
    processIIR(channel.hpf);
  }
  else if(channel.mode == BSF) //band stop
  {
    processIIR(channel.lpf);
    processIIR(channel.hpf);
    IIRsumChannels(channel.hpf, channel.lpf, I2S_DMA_BUF_LEN);
  }
    
}

void IIRsumChannels(iirConfig &one, iirConfig &two, int len) //sums the output buffers of two iirConfigs, divided by two to prevent overflows. Stores the result in one.
{
  for(int i = 0; i < len; i++)
  {
    one.dst[i] = (one.dst[i] >> 1) + (two.dst[i] >> 1); //prevents overflow.
  }
}
