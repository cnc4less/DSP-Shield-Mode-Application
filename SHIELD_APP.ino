/*
	 Multipurpose DSP App.
	 Takes all sorts of commands from Arduino via SPI and performs signal processing.
 */
#include "Audio.h"
#include "filter.h"
#include "OLED.h"
#include "SPI.h"
#include "SD.h"

//IIR Filtering
#include "filterIir.h"

//FIR Filtering
#include "filterFir.h"

//general defines
#define CHAN_LEFT 0
#define CHAN_RIGHT 1
#define CHAN_BOTH 2

#define LOW_PASS 1
#define HIGH_PASS 2
#define BAND_PASS 3
#define BAND_STOP 4

//DDS generation
#include "ddsCode.h"

//mailbox message toolboxes
#include "mailbox.h"
#include "messageStateMachine.h" //extension for the mailbox library that enables the reading of a message one byte at a time instead of all at once.
messageStateData arduinoMessageState; //struct with state data for message state machine.

//spectrum analysis
#include "fftCode.h"

//Sample buffers

  // input samples
  int filterIn1[I2S_DMA_BUF_LEN];
  int filterIn2[I2S_DMA_BUF_LEN];
  
  // intermeidate processing samples
  int filterInt1[I2S_DMA_BUF_LEN];
  int filterInt2[I2S_DMA_BUF_LEN];
  
  // output samples
  int filterOut1[I2S_DMA_BUF_LEN];
  int filterOut2[I2S_DMA_BUF_LEN];

// flag to indicate that input samples are ready to be filtered
unsigned short readyForFilter = 0;

//  flag to indicate samples are processed
unsigned short filterBufAvailable = 0;

// flag to switch between the data buffers of the Audio library
unsigned short writeBufIndex = 0;

// input source
int inputCodec = 0;

inline void setXF(bool togg) //macro to toggle XF LED on DSP Shield
{
  if(togg)
    asm(" BIT(ST1, #13) = #1");
  else
    asm(" BIT(ST1, #13) = #0");
}

// DMA Interrupt Service Routine
interrupt void dmaIsr(void)
{
    unsigned short ifrValue;
    ifrValue = DMA.getInterruptStatus();
    if ((ifrValue >> DMA_CHAN_ReadR) & 0x01)
    {
      /* Data read from codec is copied to filter input buffers.
           Filtering is done after configuring DMA for next block of transfer
           ensuring no data loss */
      if(inputCodec == 0)
      {
            fillShortBuf(filterIn1, 0, I2S_DMA_BUF_LEN);
            fillShortBuf(filterIn2, 0, I2S_DMA_BUF_LEN);
      }
      else if(inputCodec == 1)
      {
        copyShortBuf(AudioC.audioInLeft[AudioC.activeInBuf],
                     filterIn1, I2S_DMA_BUF_LEN);
        copyShortBuf(AudioC.audioInRight[AudioC.activeInBuf],
                     filterIn2, I2S_DMA_BUF_LEN);
      }
        readyForFilter = 1;
    }
    else if ((ifrValue >> DMA_CHAN_WriteR) & 0x01)
    {
        if (filterBufAvailable)
        {
            /* Filtered buffers need to be copied to audio out buffers as
               audio library is configured for non-loopback mode */
            writeBufIndex = (AudioC.activeOutBuf == FALSE)? TRUE: FALSE;
            copyShortBuf(iirL.hpf.dst, AudioC.audioOutLeft[writeBufIndex],
                         I2S_DMA_BUF_LEN);
            copyShortBuf(iirR.hpf.dst, AudioC.audioOutRight[writeBufIndex],
                         I2S_DMA_BUF_LEN);
            filterBufAvailable = 0;
        }
    }

    /* Calling AudioC.isrDma() will copy the buffers to Audio Out of the Codec,
     * initiates next DMA transfers of Audio samples to and from the Codec
     */
    AudioC.isrDma();

    // Check if filter buffers are ready. No filtering required for write interrupt
    if (readyForFilter)
    {
        readyForFilter = 0;

        //DDS Generation
        ddsGen(ddsConfigLeft, filterIn1, I2S_DMA_BUF_LEN);
        ddsGen(ddsConfigRight, filterIn2, I2S_DMA_BUF_LEN);

        //IIR Filtering
        IIRProcessChannel(iirL);
        IIRProcessChannel(iirR);

        if(FIRTagL) //FIR Filtering
        {
          // Filter Left Audio Channel
          filter_fir(iirL.hpf.dst, FIRcoeffsL, iirL.hpf.dst, delayBufferL, I2S_DMA_BUF_LEN, filterLen);
        }

        if(FIRTagR) //FIR Filtering
        {
          // Filter Right Audio Channel
          filter_fir(iirR.hpf.dst, FIRcoeffsR, iirR.hpf.dst, delayBufferR, I2S_DMA_BUF_LEN, filterLen);
        }
        filterBufAvailable = 1; //send output data on next interrupt
        
        //FFT handling
        updateSpectrumPointer(fftConfigLeft, filterIn1, iirL.hpf.dst, (int **) AudioC.audioInLeft); //pick the left source buffer
        spectrum(fftConfigLeft); //generate FFT'd spectrum
        updateSpectrumPointer(fftConfigRight, filterIn2, iirR.hpf.dst, (int **) AudioC.audioInRight); //pick the right source buffer
        spectrum(fftConfigRight); //generate FFT'd spectrum
    }
}
// Initializes OLED and Audio modules 
void setup()
{
    int status;
    int index;

    //set up I/0
    pinMode(LED0, OUTPUT);
    pinMode(LED1, OUTPUT);
    pinMode(LED2, OUTPUT);
    
    pinMode(ARD_I2C_EN, OUTPUT);
    digitalWrite(ARD_I2C_EN, LOW);
    
    digitalWrite(LED0, LOW);
    digitalWrite(LED1, LOW);
    digitalWrite(LED2, LOW);
    
    //Initialize OLED module for status display	
    disp.oledInit();
    disp.clear();
    disp.flip();    
    disp.setline(0);
    disp.print("Shield App");
    
    // Clear all the data buffers
    fillShortBuf(filterIn1, 0, I2S_DMA_BUF_LEN);    
    fillShortBuf(filterIn2, 0, I2S_DMA_BUF_LEN);
    fillShortBuf(filterOut1, 0, I2S_DMA_BUF_LEN);
    fillShortBuf(filterOut2, 0, I2S_DMA_BUF_LEN);
    fillShortBuf(filterInt1, 0, I2S_DMA_BUF_LEN);    
    fillShortBuf(filterInt2, 0, I2S_DMA_BUF_LEN);
    
    /* Clear the delay buffers, which will be used by the FIR filtering
       algorithm, These buffers need to be initialized to all zeroes in the
       beginning of the FIR filtering algorithm */
    fillShortBuf(delayBufferL, 0, (FILTER_LENGTH_MAX + 2));       
    fillShortBuf(delayBufferR, 0, (FILTER_LENGTH_MAX + 2));       
    
    /* Audio library is configured for non-loopback mode. Gives enough time for
       FIR filter processing in ISR */
    shieldMailbox.begin(SPI_MASTER, readFilter);
    
    //initialize SD Card
    status = SD.begin(1);
    status = AudioC.Audio(TRUE);
    AudioC.setSamplingRate(SAMPLING_RATE_44_KHZ);
    if (status == 0)
    {
        AudioC.attachIntr(dmaIsr);
    }  
    AudioC.setOutputVolume(94);

    //initialize DDS module for both left and right channels
    ddsConfigInit(ddsConfigLeft);
    ddsConfigInit(ddsConfigRight);
    
    //Initialize FFT for both channels
    fftConfigLeft = FFTInit();
    fftConfigRight = FFTInit();
    
    //set up IIR channels
    iirL = newIIRChannel(IIRdelayBufferL_L, IIRdelayBufferL_H, IIRcoeffsL_L, IIRcoeffsL_H);
    iirR = newIIRChannel(IIRdelayBufferR_L, IIRdelayBufferR_H, IIRcoeffsR_L, IIRcoeffsR_H);
   
    //configure for no filter mode, indicate the three prebuilt buffers for each channel.
    configureIIRChannel(iirL,ALL_PASS,filterIn1, filterOut1, filterInt1); 
    configureIIRChannel(iirR,ALL_PASS,filterIn2, filterOut2, filterInt2);
    
    //initialize mailbox per byte state machine.
    arduinoMessageState = initMessageStateData(arduinoMessageState);
   
}

int heartbeat = 0;
void loop()
{
  if(heartbeat == 1)
  {
    heartbeat = 0;
  }
  else
  {
    heartbeat = 1;
  }
  digitalWrite(LED1, heartbeat);
  
  if(messageStateMachine(arduinoMessageState))
  {
    readFilter();
  }
  sendSpectrum(fftConfigLeft); //send the spectrum if needed.
  sendSpectrum(fftConfigRight);
  delayMicroseconds(10);
}

int ledBlink = 0;
void readFilter()
{
   //first get the key tags that are present in every message
   int command = (shieldMailbox.inbox[1]<<8) + shieldMailbox.inbox[0];
   int channel = (shieldMailbox.inbox[3]<<8) + shieldMailbox.inbox[2];
   //parse the command, based on which command we recieved.
   switch(command) {
   case 1: //fir load direct, syntax is: <int command><FILTER_LENGTH x int coefficients>
     FIRRecieve(channel);
     break;
   case 2: //2 = FIR LOW_PASS, 3 = FIR HIGH_PASS, syntax is: <int command><int cutoff>
   case 3: 
     FIRLoad(channel, command);
     break;
   case 4: //4 = FIR BANDPASS, 5 = FIR NOTCH, syntax is: <int command><int lower bound><int upper bound>
   case 5:
     FIRLoad(channel, command);
     break;
   case 6: //FIR disable.
     firDisable(channel);
     break;
   case 7: //IIR Receive LOW_PASS/HIGH_PASS only
     IIRRecieve(channel);
     break;
   case 8: //IIR Load. Next byte is Type (butter, bessel, etc) then following byte is Pass (high low etc))
     //CODE HERE
     break;   
   case 9: //IIR toggle
      if(iirL.lpf.enabled == 0)
       iirL.lpf.enabled = 1;
     else
       iirL.lpf.enabled = 0;    
      if(iirR.lpf.enabled == 0)
       iirR.lpf.enabled = 1;
     else
       iirR.lpf.enabled = 0;    
     break;
   case 10: //initialize DDS
     ddsToneStart(channel, command);
     break;
   case 11: //DDS Stop.
     if(channel == CHAN_LEFT)
     {
       ddsConfigLeft.enable = 0;
     }
     else if(channel == CHAN_RIGHT)
     {
       ddsConfigRight.enable = 0;
     }
     else if(channel == CHAN_BOTH)
     {
       ddsConfigLeft.enable = 0;
       ddsConfigRight.enable = 0;
     }
     break;
   case 12: //DDS chirp Start. Takes frequency, channel, amplitude.
       ddsChirpStart(channel, command);
       break;
   case 13: //spectrum initialize
     setXF(true);
     initializeFFTSpectrum(channel);
     break;
   case 14: //diable spectrum;
     disableFFTSpectrum(channel);
     break;
   case 15: //enable audio codec input
     inputCodec = 1;
     break;
   case 16: //diable audio codec input
     inputCodec = 0;
     break;
   case 17: //OLED Display print
     oledPrintMessage();
     break;
   case 18: //dual IIR filter transmission.
     IIRRecieveDual(channel);
     break;
   }
  //friendly messaged recieve LED toggle.
  if(ledBlink)
  {
    ledBlink = 0;
  }
  else
  {
    ledBlink = 1;
  }
  digitalWrite(LED0, ledBlink);
}

void oledPrintMessage()
{
   disp.setline(1);
   disp.clear(1);
   disp.print(shieldMailbox.inbox+4); //print the recieved string
   if(shieldMailbox.inboxSize-4 > 15)
   {
     disp.scrollDisplayLeft(1);
   }
   else
   {
     disp.noAutoscroll();
   }
}

void IIRRecieve(int channel)
{
     int order = (shieldMailbox.inbox[5]<<8) + shieldMailbox.inbox[4];
     int dest = (shieldMailbox.inbox[7]<<8) + shieldMailbox.inbox[6];
     int* newData = (int*) malloc(shieldMailbox.inboxSize/2);  
     //Working read via serial code
     for(int i = 8; i < shieldMailbox.inboxSize; i = i + 2) //copy recieved coefficients to buffer.
     {
       newData[i/2 - 4] = ((shieldMailbox.inbox[i+1]<<8) + shieldMailbox.inbox[i]);
     }
     
     if(dest == LOW_PASS)
     {
       int newDataLen = shieldMailbox.inboxSize/2 - 4;
       //int newDataLen = order/2*COEFFS_PER_BIQUAD;
       if(channel == CHAN_LEFT) //channel 0 == left
       {
         iirL.lpf.order = order;
         memcpy(iirL.lpf.coeffs, newData, newDataLen); //copy into low pass coefficients
         configureIIRChannel(iirL,LOW_PASS,filterIn1, filterOut1, filterInt1);  //configure blocks
       }
       else if (channel == CHAN_RIGHT) //channel 1 == right
       {
         iirR.lpf.order = order;
         memcpy(iirR.lpf.coeffs, newData, newDataLen); //copy into low pass coefficients
         configureIIRChannel(iirR,LOW_PASS,filterIn2, filterOut2, filterInt2); //configure blocks
       }
       else if (channel == CHAN_BOTH) //channel 2 == both
       {
         iirL.lpf.order = order;
         memcpy(iirL.lpf.coeffs, newData, newDataLen); //copy into low pass coefficients
         
         iirR.lpf.order = order;
         memcpy(iirR.lpf.coeffs, newData, newDataLen); //copy into low pass coefficients

         configureIIRChannel(iirL,LOW_PASS,filterIn1, filterOut1, filterInt1); //configure blocks
         configureIIRChannel(iirR,LOW_PASS,filterIn2, filterOut2, filterInt2);
       }
       
     }
     else if(dest == HIGH_PASS)
     {
       int newDataLen = shieldMailbox.inboxSize/2 - 4;
       //int newDataLen = order/2*COEFFS_PER_BIQUAD;
       if(channel == CHAN_LEFT) //channel 0 == left
       {
         iirL.hpf.order = order;
         memcpy(iirL.hpf.coeffs, newData, newDataLen); //copy into low pass coefficients
         configureIIRChannel(iirL,HIGH_PASS,filterIn1, filterOut1, filterInt1);  //configure blocks
       }
       else if (channel == CHAN_RIGHT) //channel 1 == right
       {
         iirR.hpf.order = order;
         memcpy(iirR.hpf.coeffs, newData, newDataLen); //copy into low pass coefficients
         configureIIRChannel(iirR,HIGH_PASS,filterIn2, filterOut2, filterInt2); //configure blocks
       }
       else if (channel == CHAN_BOTH) //channel 2 == both
       {
         iirL.hpf.order = order;
         memcpy(iirL.hpf.coeffs, newData, newDataLen); //copy into low pass coefficients
         
         iirR.hpf.order = order;
         memcpy(iirR.hpf.coeffs, newData, newDataLen); //copy into low pass coefficients

         configureIIRChannel(iirL,HIGH_PASS,filterIn1, filterOut1, filterInt1); //configure blocks
         configureIIRChannel(iirR,HIGH_PASS,filterIn2, filterOut2, filterInt2);
       }
       
     }
     free(newData);
}

void IIRRecieveDual(int channel)
{
     int order1 = (shieldMailbox.inbox[5]<<8) + shieldMailbox.inbox[4];
     int order2 = (shieldMailbox.inbox[7]<<8) + shieldMailbox.inbox[6];
     int filterType = (shieldMailbox.inbox[9]<<8) + shieldMailbox.inbox[8];
     int* newData = (int*) malloc(shieldMailbox.inboxSize/2);  
     //Working read via serial code
     for(int i = 10; i < order1*COEFFS_PER_BIQUAD+10; i = i + 2) //copy recieved coefficients to buffer.
     {
       newData[i/2 - 5] = ((shieldMailbox.inbox[i+1]<<8) + shieldMailbox.inbox[i]);
     }
      
     
     if (channel == CHAN_LEFT) //channel 0 == left
     {
       iirL.lpf.order = order1;
       memcpy(iirL.lpf.coeffs, newData, order1/2*COEFFS_PER_BIQUAD);
     }
     else if (channel == CHAN_RIGHT) //channel 1 == right
     {
       iirR.lpf.order = order1;
       memcpy(iirR.lpf.coeffs, newData, order1/2*COEFFS_PER_BIQUAD);
     }
     else if (channel == CHAN_BOTH) //channel 2 == both
     {
       iirL.lpf.order = order1;
       memcpy(iirL.lpf.coeffs, newData, order1/2*COEFFS_PER_BIQUAD);
       
       iirR.lpf.order = order1;
       memcpy(iirR.lpf.coeffs, newData, order1/2*COEFFS_PER_BIQUAD);
     }
     
     for(int i = 10+order1*COEFFS_PER_BIQUAD; i < shieldMailbox.inboxSize; i = i + 2) //copy recieved coefficients to buffer.
     {
       newData[i/2 - 5 - order1/2*COEFFS_PER_BIQUAD] = ((shieldMailbox.inbox[i+1]<<8) + shieldMailbox.inbox[i]);
     }

     if (channel == CHAN_LEFT) //channel 0 == left
     {
       iirL.hpf.order = order2;
       memcpy(iirL.hpf.coeffs, newData, order2/2*COEFFS_PER_BIQUAD);
       configureIIRChannel(iirL,filterType,filterIn1, filterOut1, filterInt1); //configure blocks
     }
     else if (channel == CHAN_RIGHT) //channel 1 == right
     {
       iirR.hpf.order = order2;
       memcpy(iirR.hpf.coeffs, newData, order2/2*COEFFS_PER_BIQUAD);
       configureIIRChannel(iirR,filterType,filterIn2, filterOut2, filterInt2); //configure blocks
     }
     else if (channel == CHAN_BOTH) //channel 2 == both
     {
       iirL.hpf.order = order2;
       memcpy(iirL.hpf.coeffs, newData, order2/2*COEFFS_PER_BIQUAD);
       
       iirR.hpf.order = order2;
       memcpy(iirR.hpf.coeffs, newData, order2/2*COEFFS_PER_BIQUAD);

       configureIIRChannel(iirL,filterType,filterIn1, filterOut1, filterInt1); //configure blocks
       configureIIRChannel(iirR,filterType,filterIn2, filterOut2, filterInt2);

     }
     //printIIRData(iirL);
     //printIIRData(iirR);
     free(newData);
}
