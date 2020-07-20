
#include <GenericTypeDefs.h>
#include "parse.h"
#include "HardwareProfile.h"
#include "ebb.h"
#include "utility.h"
#include "isr.h"
#include "fifo.h"
#include "Compiler.h"
#include "servo.h"
#include "math.h"
#include "stepper.h"


// This is the value that gets multiplied by Steps/Duration to compute
// the StepAdd values.
#define OVERFLOW_MUL            (0x8000 / HIGH_ISR_TICKS_PER_MS)

/* These values hold the global step position of each axis */
volatile INT32 globalStepCounter1;
volatile INT32 globalStepCounter2;
volatile INT32 globalStepCounter3;

// When FALSE, we skip parameter checks for motor move commands so they can run faster
BOOL gLimitChecks = TRUE;

/* Local function definitions */
void clear_StepCounters(void);

// Enable Motor
// Usage: EM,<EnableAxis1>,<EnableAxis2><CR>
// Everything after EnableAxis1 is optional
// Each parameter can have a value of
//    0 to disable that motor driver
// FOR OLD DRIVER CHIP (A3967) - can do separate enables for each axis
//    1 to enable the driver in 1/8th step mode
//    2 to enable the driver in 1/4 step mode
//    3 to enable the driver in 1/2 step mode
//    4 to enable the driver in full step mode
// FOR NEW DRIVER CHIP (A4988/A4983)
// (only first parameter applies, and it applies to both drivers)
//    1 to enable the driver in 1/16th step mode
//    2 to enable the driver in 1/8 step mode
//    3 to enable the driver in 1/4 step mode
//    4 to enable the driver in 1/2 step mode
//    5 to enable the driver in full step mode
// If you disable a motor, it goes 'limp' (we clear the ENABLE pin on that motor's
// driver chip)
// Note that when using 0 or 1 for a parameter, you can use both axis even
// on a 'new' driver chip board. (i.e. EM,0,1 will disable motor 1 and enable 2)
// Note that the MSx lines do not come to any headers, so even when an external
// source is controlling the drivers, the PIC still needs to control the
// MSx lines.
void parseEMCommand(void)
{
  unsigned char EA1, EA2;
  ExtractReturnType RetVal;

  // Extract each of the values.
  RetVal = extract_number (kUINT8, &EA1, kREQUIRED);
  if (kEXTRACT_OK == RetVal)
  {
    // Bail if we got a conversion error
    if (error_byte)
    {
      return;
    }
/// TODO: Come back and fix this for 3BB
#if defined(BOARD_EBB)
    if (
      (DriverConfiguration == PIC_CONTROLS_DRIVERS)
      ||
      (DriverConfiguration == EXTERNAL_CONTROLS_DRIVERS)
    )
    {
      if (EA1 > 0)
      {
        if (DriverConfiguration == PIC_CONTROLS_DRIVERS)
        {
          Enable1IO = ENABLE_MOTOR;
          RCServoPowerIO = RCSERVO_POWER_ON;
          gRCServoPoweroffCounterMS = gRCServoPoweroffCounterReloadMS;
        }
        if (EA1 == 1)
        {
          MS1_IO = 1;
          MS2_IO = 1;
          MS3_IO = 1;
        }
        if (EA1 == 2)
        {
          MS1_IO = 1;
          MS2_IO = 1;
          MS3_IO = 0;
        }
        if (EA1 == 3)
        {
          MS1_IO = 0;
          MS2_IO = 1;
          MS3_IO = 0;
        }
        if (EA1 == 4)
        {
          MS1_IO = 1;
          MS2_IO = 0;
          MS3_IO = 0;
        }
        if (EA1 == 5)
        {
          MS1_IO = 0;
          MS2_IO = 0;
          MS3_IO = 0;
        }
      }
      else
      {
        if (DriverConfiguration == PIC_CONTROLS_DRIVERS)
                {
        Enable1IO = DISABLE_MOTOR;
                }
      }
    }
    else if (DriverConfiguration == PIC_CONTROLS_EXTERNAL)
    {
      if (EA1 > 0)
      {
        Enable1AltIO = ENABLE_MOTOR;
        RCServoPowerIO = RCSERVO_POWER_ON;
        gRCServoPoweroffCounterMS = gRCServoPoweroffCounterReloadMS;
      }
      else
      {
        Enable1AltIO = DISABLE_MOTOR;
      }
    }
#endif
  }

  RetVal = extract_number (kUINT8, &EA2, kOPTIONAL);
  if (kEXTRACT_OK == RetVal)
  {
    // Bail if we got a conversion error
    if (error_byte)
    {
      return;
    }
#if defined(BOARD_EBB)
    if (DriverConfiguration == PIC_CONTROLS_DRIVERS)
    {
      if (EA2 > 0)
      {
        Enable2IO = ENABLE_MOTOR;
        RCServoPowerIO = RCSERVO_POWER_ON;
        gRCServoPoweroffCounterMS = gRCServoPoweroffCounterReloadMS;
      }
      else
      {
        Enable2IO = DISABLE_MOTOR;
      }
    }
    else if (DriverConfiguration == PIC_CONTROLS_EXTERNAL)
    {
      if (EA2 > 0)
      {
        Enable2AltIO = ENABLE_MOTOR;
        RCServoPowerIO = RCSERVO_POWER_ON;
        gRCServoPoweroffCounterMS = gRCServoPoweroffCounterReloadMS;
      }
      else
      {
        Enable2AltIO = DISABLE_MOTOR;
      }
    }
#endif
  }

  // Always clear the step counts if motors are enabled/disabled or 
  // resolution is changed.
  clear_StepCounters();
  
  print_ack();
}

// RM command
// For Run Motor - allows completely independent running of the three stepper motors
/// TODO : do we even need this?
void parseRMCommand(void)
{
}


// The Stepper Motor command
// Usage: SM,<move_duration>,<axis1_steps>,<axis2_steps>,<axis3_steps><CR>
// <move_duration> is a number from 1 to 16777215, indicating the number of milliseconds this move should take
// <axisX_steps> is a signed 24 bit number indicating how many steps (and what direction) the axis should take
// NOTE1: <axis2_steps> and <axis3_steps> are optional and can be left off
// If the *BB can not make the move in the specified time, it will take as long as it needs to at max speed
// i.e. SM,1,1000 will not produce 1000steps in 1ms. Instead, it will take 40ms (25KHz max step rate)
// NOTE2: If you specify zero steps for the the three axis values, then you effectively create a delay. Use for small
// pauses before raising or lowering the pen, for example.
void parseSMCommand(void)
{
  UINT32 Duration = 0;
  INT32 A1Steps = 0, A2Steps = 0, A3Steps = 0;
  INT32 Steps = 0;

  // Extract each of the values.
  extract_number (kUINT32, &Duration, kREQUIRED);
  extract_number (kINT32, &A1Steps, kREQUIRED);
  extract_number (kINT32, &A2Steps, kOPTIONAL);
  extract_number (kINT32, &A3Steps, kOPTIONAL);

  if (gLimitChecks)
  {
    // Check for invalid duration
    if (Duration == 0) 
    {
      bitset (error_byte, kERROR_BYTE_PARAMETER_OUTSIDE_LIMIT);
    }
    // Bail if we got a conversion error
    if (error_byte)
    {
      return;
    }
    // Limit each parameter to just 3 bytes
    if (Duration > 0xFFFFFF) 
    {
      printf((far rom char *)"!0 Err: <move_duration> larger than 16777215 ms.\n\r");
      return;
    }
    // Check for too-fast step request (>25KHz)
    // First get absolute value of steps, then check if it's asking for >25KHz
    if (A1Steps > 0) 
    {
      Steps = A1Steps;
    }
    else 
    {
      Steps = -A1Steps;
    }
    if (Steps > 0xFFFFFF) 
    {
      printf((far rom char *)"!0 Err: <axis1> larger than 16777215 steps.\n\r");
      return;
    }
    // Check for too fast
    if ((Steps/Duration) > HIGH_ISR_TICKS_PER_MS) 
    {
      printf((far rom char *)"!0 Err: <axis1> step rate > 25K steps/second.\n\r");
      return;
    }
    // And check for too slow
    if ((Duration/1311) >= Steps && Steps != 0) 
    {
      printf((far rom char *)"!0 Err: <axis1> step rate < 1.31Hz.\n\r");
      return;
    }

    if (A2Steps > 0) 
    {
      Steps = A2Steps;
    }
    else {
      Steps = -A2Steps;
    }    

    if (Steps > 0xFFFFFF) 
    {
      printf((far rom char *)"!0 Err: <axis2> larger than 16777215 steps.\n\r");
      return;
    }
    if ((Steps/Duration) > HIGH_ISR_TICKS_PER_MS) 
    {
      printf((far rom char *)"!0 Err: <axis2> step rate > 25K steps/second.\n\r");
      return;
    }
    if ((Duration/1311) >= Steps && Steps != 0) 
    {
      printf((far rom char *)"!0 Err: <axis2> step rate < 1.31Hz.\n\r");
      return;
    }

    if (A3Steps > 0) 
    {
      Steps = A3Steps;
    }
    else {
      Steps = -A3Steps;
    }    

    if (Steps > 0xFFFFFF) 
    {
      printf((far rom char *)"!0 Err: <axis3> larger than 16777215 steps.\n\r");
      return;
    }
    if ((Steps/Duration) > HIGH_ISR_TICKS_PER_MS) 
    {
      printf((far rom char *)"!0 Err: <axis3> step rate > 25K steps/second.\n\r");
      return;
    }
    if ((Duration/1311) >= Steps && Steps != 0) 
    {
      printf((far rom char *)"!0 Err: <axis3> step rate < 1.31Hz.\n\r");
      return;
    }
  }

  // If we get here, we know that step rate for both A1 and A2 is
  // between 25KHz and 1.31Hz which are the limits of what EBB can do.
  process_SM(Duration, A1Steps, A2Steps, A3Steps);

  if (g_ack_enable)
  {
    print_ack();
  }
}


// The Accelerated Motion command
// Usage: SM,<inital_velocity>,<final_velocity>,<axis1_steps>,<axis2_steps><CR>
// <inital_velocity> is a number from 1 to 10000 in steps/second indicating the initial velocity
// <final_velocity> is a number from 1 to 10000 in steps/second indicating the final velocity
// <axisX_steps> is a signed 24 bit number indicating how many steps (and what direction) the axis should take
// Note that the two velocities are of the combined move - i.e. the tip of the pen, not the individual
// axies velocities.
void parseAMCommand(void)
{
  UINT32 temp = 0;
  UINT16 VelocityInital = 0;
  UINT16 VelocityFinal = 0;
  INT32 A1Steps = 0, A2Steps = 0;
  UINT32 Duration = 0;
  UINT32 Distance;
  float distance_temp;
  float accel_temp;

  // Extract each of the values.
  extract_number (kUINT32, &VelocityInital, kREQUIRED);
  extract_number (kUINT32, &VelocityFinal, kREQUIRED);
  extract_number (kINT32, &A1Steps, kREQUIRED);
  extract_number (kINT32, &A2Steps, kREQUIRED);

  // Check for too-fast step request (>25KHz)
  if (VelocityInital > 25000)
  {
    printf((far rom char *)"!0 Err: <velocity_initial> larger than 25000.\n\r");
    return;
  }
  if (VelocityFinal > 25000)
  {
    printf((far rom char *)"!0 Err: <velocity_final> larger than 25000.\n\r");
    return;
  }
  if (VelocityInital < 4)
  {
    printf((far rom char *)"!0 Err: <velocity_initial> less than 4.\n\r");
    return;
  }
  if (VelocityFinal < 4)
  {
    printf((far rom char *)"!0 Err: <velocity_final> less than 4.\n\r");
    return;
  }
    
  // Bail if we got a conversion error
  if (error_byte)
  {
    return;
  }
    
  // Compute total pen distance
  // fprint(ftemp);
  Distance = (UINT32)(sqrt((float)((A1Steps * A1Steps) + (A2Steps * A2Steps))));
    
  // For debug
  //printf((far rom char *)"Distance= %lu\n\r", Distance);

  WaitForRoomInFIFO();

  FIFO_DirBits[FIFOIn] = 0;

  // Always enable both motors when we want to move them
/// TODO: Fix this for 3BB motor enable
#if defined(BOARD_EBB)
  Enable1IO = ENABLE_MOTOR;
  Enable2IO = ENABLE_MOTOR;
  RCServoPowerIO = RCSERVO_POWER_ON;
  gRCServoPoweroffCounterMS = gRCServoPoweroffCounterReloadMS;
#endif
  
  // First, set the direction bits
  if (A1Steps < 0)
  {
    FIFO_DirBits[FIFOIn] = DIR1_BIT;
    A1Steps = -A1Steps;
  }
  if (A2Steps < 0)
  {
    FIFO_DirBits[FIFOIn] = FIFO_DirBits[FIFOIn] | DIR2_BIT;
    A2Steps = -A2Steps;
  }

  if (A1Steps > 0xFFFFFF) {
    printf((far rom char *)"!0 Err: <axis1> larger than 16777215 steps.\n\r");
    return;
  }
  if (A2Steps > 0xFFFFFF) {
    printf((far rom char *)"!0 Err: <axis2> larger than 16777215 steps.\n\r");
    return;
  }

  // To compute StepAdd values from Duration.
  // A1Stp is from 0x000001 to 0xFFFFFF.
  // HIGH_ISR_TICKS_PER_MS = 25
  // Duration is from 0x000001 to 0xFFFFFF.
  // temp needs to be from 0x0001 to 0x7FFF.
  // Temp is added to accumulator every 25KHz. So slowest step rate
  // we can do is 1 step every 25KHz / 0x7FFF or 1 every 763mS. 
  // Fastest step rate is obviously 25KHz.
  // If A1Stp is 1, then duration must be 763 or less.
  // If A1Stp is 2, then duration must be 763 * 2 or less.
  // If A1Stp is 0xFFFFFF, then duration must be at least 671088.

  // Test values
  // Duration = 2000;
  // Distance = 600;
  // Acceleration = 200;
  // VelocityInital = 100;
  // VelocityFinal = 500;
    
  /* Compute StepAdd Axis 1 Initial */
  //temp = ((UINT32)A1Steps*(((UINT32)VelocityInital * (UINT32)0x8000)/(UINT32)25000)/(UINT32)Distance);
  //printf((far rom char *)"VelocityInital = %d\n\r", VelocityInital);
  //printf((far rom char *)"Distance = %ld\n\r", Distance);
  //temp = (UINT32)((float)A1Steps*(((float)VelocityInital * (float)0x80000000UL)/(float)25000)/(float)Distance);
  distance_temp = ((float)VelocityInital * 85899.34592)/Distance;
  //printf((far rom char *)"distance_temp =");
  //fprint(distance_temp);
  //ftemp = distance_temp * A1Steps;
  //fprint(ftemp);
  //temp = (UINT32)ftemp;

  /* Amount to add to accumulator each 25KHz */
  FIFO_StepAdd0[FIFOIn] = (UINT32)(distance_temp * (float)A1Steps);

  // For debug
  //printf((far rom char *)"SAxi = %lu\n\r", CommandFIFO[0].StepAdd[0]);

  /* Total number of steps for this axis for this move */
  FIFO_G2[FIFOIn].StepsCounter0 = A1Steps;

  //ftemp = (float)VelocityFinal * 2147483648.0;
  //fprint(ftemp);
  //ftemp = ftemp / 25000;
  //fprint(ftemp);
  //ftemp = ftemp / Distance;
  //fprint(ftemp);
  //ftemp = ftemp * A1Steps;
  //fprint(ftemp);
  //temp = (UINT32)ftemp;

  // For debug
  //printf((far rom char *)"SAxf = %lu\n\r", temp);
    
  /* Compute StepAddInc for axis 1 */
  accel_temp = (((float)VelocityFinal * (float)VelocityFinal) - ((float)VelocityInital * (float)VelocityInital))/((float)Distance * (float)Distance * 2);
  //Accel1 = ((float)A1Steps * accel_temp);
  //printf((far rom char *)"accel_temp : ");
  //fprint(accel_temp);
  //stemp = (INT32)((Accel1 * (float)0x8000 * (float)0x10000)/((float)25000 * (float)25000));
  //stemp = (INT32)(Accel1 * 343.59738);
  //printf((far rom char *)"SAxinc = %ld\n\r", stemp);

  /* Amount to add to StepAdd each 25KHz */
  FIFO_G5[FIFOIn].StepAddInc0 = (INT32)(((float)A1Steps * accel_temp) * 3.435921);

  /* Compute StepAdd Axis 2 Initial */
  // temp = ((UINT32)A2Steps*(((UINT32)VelocityInital * (UINT32)0x8000)/(UINT32)25000)/(UINT32)Distance);
  // temp = (UINT32)((float)A2Steps*(((float)VelocityInital * (float)0x80000000)/(float)25000)/(float)Distance);

  //printf((far rom char *)"VelocityInital = %d\n\r", VelocityInital);
  //printf((far rom char *)"Distance = %ld\n\r", Distance);
  //    temp = (UINT32)((float)A1Steps*(((float)VelocityInital * (float)0x80000000UL)/(float)25000)/(float)Distance);
  //    ftemp = (float)VelocityInital * 2147483648.0;
  //    fprint(ftemp);
  //    ftemp = ftemp / 25000;
  //    fprint(ftemp);
  //    ftemp = ftemp / Distance;
  //    fprint(ftemp);
  //    ftemp = ftemp * A2Steps;
  //    fprint(ftemp);
  //    temp = (UINT32)ftemp;
    
  // For debug
  //printf((far rom char *)"SAyi = %lu\n\r", temp);

  FIFO_StepAdd1[FIFOIn] = (UINT32)(distance_temp * A2Steps);
  FIFO_G3[FIFOIn].StepsCounter1 = A2Steps;
    
  /* Compute StepAddInc for axis 2 */
  //    Accel2 = ((float)A2Steps * accel_temp);
  //printf((far rom char *)"Accel2 : ");
  //fprint(Accel2);
  //    stemp = (INT32)((Accel2 * (float)0x8000 * (float)0x10000)/((float)25000 * (float)25000));
  //    stemp = (INT32)(((float)A2Steps * accel_temp) * 343.59738);
    
  FIFO_StepAddInc1[FIFOIn] = (INT32)(((float)A2Steps * accel_temp) * 3.435921);

  if (VelocityInital != VelocityFinal && FIFO_G5[FIFOIn].StepAddInc0 == 0 && FIFO_G2[FIFOIn].StepsCounter0 > 0)
  {
     printf((far rom char *)"!0 Err: <axis1> acceleration value is 0.\n\r");
     return;
  }
  if (VelocityInital != VelocityFinal && FIFO_StepAddInc1[FIFOIn] == 0 && FIFO_G3[FIFOIn].StepsCounter1 > 0)
  {
     printf((far rom char *)"!0 Err: <axis2> acceleration value is 0.\n\r");
     return;
  }

  FIFO_Command[FIFOIn] = COMMAND_MOTOR_MOVE;
  
  fifo_Inc();
    
  print_ack();
}

// Low Level Move command
// Usage: LM,<StepAdd1>,<StepsCounter1>,<StepAddInc1>,<StepAdd2>,<StepsCounter2>,<StepAddInc2><CR>
void parseLMCommand(void)
{
  UINT32 StepAdd1, StepAddInc1, StepAdd2, StepAddInc2 = 0;
  INT32 StepsCounter1, StepsCounter2 = 0;
    
  // Extract each of the values.
  extract_number (kUINT32, &StepAdd1, kREQUIRED);
  extract_number (kINT32,  &StepsCounter1, kREQUIRED);
  extract_number (kINT32, &StepAddInc1, kREQUIRED);
  extract_number (kUINT32, &StepAdd2, kREQUIRED);
  extract_number (kINT32,  &StepsCounter2, kREQUIRED);
  extract_number (kINT32, &StepAddInc2, kREQUIRED);

  // Bail if we got a conversion error
  if (error_byte)
  {
      return;
  }

  /* Quickly eliminate obvious invalid parameter combinations,
   * like LM,0,0,0,0,0,0. Or LM,0,1000,0,100000,0,100 GH issue #78 */
  if (
      (
        ((StepAdd1 == 0) && (StepAddInc1 == 0))
        ||
        (StepsCounter1 == 0)
      )
      &&
      (
        ((StepAdd2 == 0) && (StepAddInc2 == 0))
        ||
        (StepsCounter2 == 0)
      )
  )
  {
    return;
  }
  
/// TODO: Get this to work for 3BB motor enable as well  
#if defined(BOARD_EBB)
  // Always enable both motors when we want to move them
  Enable1IO = ENABLE_MOTOR;
  Enable2IO = ENABLE_MOTOR;
  RCServoPowerIO = RCSERVO_POWER_ON;
  gRCServoPoweroffCounterMS = gRCServoPoweroffCounterReloadMS;
#endif
  
  // Spin here until there's space in the fifo
  WaitForRoomInFIFO();

  FIFO_DirBits[FIFOIn] = 0;

  // First, set the direction bits
  if (StepsCounter1 < 0)
  {
    FIFO_DirBits[FIFOIn] = FIFO_DirBits[FIFOIn] | DIR1_BIT;
    StepsCounter1 = -StepsCounter1;
  }
  if (StepsCounter2 < 0)
  {
    FIFO_DirBits[FIFOIn] = FIFO_DirBits[FIFOIn] | DIR2_BIT;
    StepsCounter2 = -StepsCounter2;
  }

  FIFO_StepAdd0[FIFOIn] = StepAdd1;
  FIFO_G2[FIFOIn].StepsCounter0 = StepsCounter1;
  FIFO_G5[FIFOIn].StepAddInc0 = StepAddInc1;
  FIFO_StepAdd1[FIFOIn] = StepAdd2;
  FIFO_G3[FIFOIn].StepsCounter1 = StepsCounter2;
  FIFO_StepAddInc1[FIFOIn] = StepAddInc2;
  FIFO_Command[FIFOIn] = COMMAND_MOTOR_MOVE;

  /* For debugging step motion , uncomment the next line */
  /*
   * printf((far rom char *)"SA1=%lu SC1=%lu SA2=%lu SC2=%lu\n\r",
          CommandFIFO[0].StepAdd[0],
          CommandFIFO[0].StepsCounter[0],
          CommandFIFO[0].StepAdd[1],
          CommandFIFO[0].StepsCounter[1]
      );
   */

  fifo_Inc();

  if (g_ack_enable)
  {
    print_ack();
  }
}



// Home the motors
// "HM,<step_rate><CR>"
// <step_rate> is the desired rate of the primary (larger) axis in steps/s.
// Use the current global step counts to "undo" the current pen position so
// it gets back to zero.
// To figure out what the duration of the move should be, look at the axis
// with more steps to move (the 'primary' axis).
// There are a couple of special cases to consider:
// 1) If <step_rate> causes a step rate that's too high on the primary axis, 
//    then just use a duration which is slow enough.
// 2) If the <step_rate> causes the non-primary axis to need to move too slowly,
//    break the move into two parts. (i.e. "dog-leg" it) First move will move
//    with a duration such that the minor axis moves at the minimum step rate.
//    Second move will move at <step_rate> but with minor axis moving zero steps.
// If the step rate is too high or too low, don't error out, just use a legal value
// May need to make 2 moves if one of the axis has few steps to go and the other has
// lots.
// When parsing this command, always wait until both the FIFO is empty and the motion 
// commands are finished. That way two SM commands can be issued back to back to take 
// care of both moves (or just one if a 'dog leg' move is needed)
//
// TODO: This code can't handle steps counts above 4,294,967 in either axis. Is
// there a way to allow it to handle steps counts up to 16,777,215 easily?
void parseHMCommand(void)
{
  UINT32 StepRate = 0;
  INT32 Steps1 = 0, Steps2 = 0, Steps3 = 0;
  INT32 AbsSteps1 = 0, AbsSteps2 = 0, AbsSteps3 = 0;
  UINT32 Duration = 0;
  UINT8 CommandExecuting = 1;
  INT32 XSteps = 0;

  // Extract the step rate.
  extract_number (kUINT32, &StepRate, kREQUIRED);

  // Wait until FIFO is completely empty
  WaitForEmptyFIFO();

  // We now know that the motors have stopped moving
  // This should not be necessary, but just in case,
  // turn off high priority interrupts breifly here to read out value that ISR uses
  INTCONbits.GIEH = 0;  // Turn high priority interrupts off
  
  // Make a local copy of the things we care about. This is how far we need to move.
  Steps1 = -globalStepCounter1;
  Steps2 = -globalStepCounter2;
  Steps3 = -globalStepCounter3;

  // Re-enable interrupts
  INTCONbits.GIEH = 1;  // Turn high priority interrupts on

  // Compute absolute value versions of steps for computation
  if (Steps1 < 0)
  {
    AbsSteps1 = -Steps1;
  }
  else
  {
    AbsSteps1 = Steps1;
  }
  if (Steps2 < 0)
  {
    AbsSteps2 = -Steps2;
  }
  else
  {
    AbsSteps2 = Steps2;
  }
  if (Steps3 < 0)
  {
    AbsSteps3 = -Steps3;
  }
  else
  {
    AbsSteps3 = Steps3;
  }
    
  // Check for too many steps to step
  if ((AbsSteps1 > 0xFFFFFF) || (AbsSteps2 > 0xFFFFFF) || (AbsSteps3 > 0xFFFFFF))
  {
    printf((far rom char *)"!0 Err: steps to home larger than 16,777,215 on at least one axis.\n\r");
    return;
  }
  
  // Compute duration based on step rate user requested. Take bigger step count to use for calculation
/// TODO : UPdate this somehow to use third axis
  if (AbsSteps1 > AbsSteps2)
  {
    Duration = (AbsSteps1 * 1000) / StepRate;
    // Axis1 is primary
    // Check for too fast 
    if ((StepRate/1000) > HIGH_ISR_TICKS_PER_MS)
    {
      printf((far rom char *)"!0 Err: HM <axis1> step rate > 25K steps/second.\n\r");
      return;
    }
    // Check for too slow, on the non-primary axis
    if ((Duration/1311) >= AbsSteps2 && AbsSteps2 != 0)
    {
      // We need to break apart the home into two moves.
      // The first will be to get the non-primary axis down to zero.
      // Recompute duration for the first move
      Duration = (AbsSteps2 * 1000) / StepRate;
      if (Steps1 > 0 && Steps2 > 0)       // C
      {
        XSteps = Steps2;
      }
      else if (Steps1 < 0 && Steps2 > 0)  // B
      {
        XSteps = -Steps2;
      }
      else if (Steps1 > 0 && Steps2 < 0)  // D
      {
        XSteps = -Steps2;
      }
      else if (Steps1 < 0 && Steps2 < 0)  // A
      {
        XSteps = Steps2;
      }
      /// TODO: Add 3rd axis?
      process_SM(Duration, XSteps, Steps2, 0);
      // Update both steps count for final move
      Steps1 = Steps1 - XSteps;
      Steps2 = 0;
      // Recompute duration
      Duration = (AbsSteps1 * 1000) / StepRate;
    }
  }
  else
  {
    Duration = (AbsSteps2 * 1000) / StepRate;        
    // Axis2 is primary
    // Check for too fast 
    if ((StepRate/1000) > HIGH_ISR_TICKS_PER_MS)
    {
      printf((far rom char *)"!0 Err: HM <axis2> step rate > 25K steps/second.\n\r");
      return;
    }
    // Check for too slow, on the non-primary axis
    if ((Duration/1311) >= AbsSteps1 && AbsSteps1 != 0)
    {
      // We need to break apart the home into two moves.
      // The first will be to get the non-primary axis down to zero.
      // Recompute duration for the first move
      Duration = (AbsSteps1 * 1000) / StepRate;
      if (Steps2 > 0 && Steps1 > 0)       // C
      {
        XSteps = Steps1;
      }
      else if (Steps2 < 0 && Steps1 > 0)  // B
      {
        XSteps = -Steps1;
      }
      else if (Steps2 > 0 && Steps1 < 0)  // D
      {
        XSteps = -Steps1;
      }
      else if (Steps2 < 0 && Steps1 < 0)  // A
      {
        XSteps = Steps1;
      }
      /// TODO: add 3rd axis?
      process_SM(Duration, Steps1, XSteps, 0);
      // Update both steps count for final move
      Steps2 = Steps2 - XSteps;
      Steps1 = 0;
      // Recompute duration
      Duration = (AbsSteps2 * 1000) / StepRate;
    }
  }

  if (Duration < 10)
  {
    Duration = 10;
  }
  //printf((far rom char *)"HM Duration=%lu SA1=%li SA2=%li\n\r",
  //  Duration,
  //  Steps1,
  //  Steps2
  //);

  // If we get here, we know that step rate for both A1 and A2 is
  // between 25KHz and 1.31Hz which are the limits of what EBB can do.
  /// TODO: Add 3rd axis?
  process_SM(Duration, Steps1, Steps2, Steps3);

  if (g_ack_enable)
  {
    print_ack();
  }
}

// The X Stepper Motor command
// Usage: XM,<move_duration>,<axisA_steps>,<axisB_steps><CR>
// <move_duration> is a number from 1 to 16777215, indicating the number of milliseconds this move should take
// <axisA_steps> and <axisB_stetsp> are signed 24 bit numbers.
// This command differs from the normal "SM" command in that it is designed to drive 'mixed-axis' geometry
// machines like H-Bot and CoreXY. Using XM will effectively call SM with Axis1 = <axisA_steps> + <axisB_steps> and
// Axis2 = <axisA_steps> - <axisB_steps>.
void parseXMCommand(void)
{
  UINT32 Duration = 0;
  INT32 A1Steps = 0, A2Steps = 0;
  INT32 ASteps = 0, BSteps = 0;
  INT32 Steps = 0;

  // Extract each of the values.
  extract_number (kUINT32, &Duration, kREQUIRED);
  extract_number (kINT32, &ASteps, kREQUIRED);
  extract_number (kINT32, &BSteps, kREQUIRED);

  // Check for invalid duration
  if (Duration == 0) 
  {
    bitset (error_byte, kERROR_BYTE_PARAMETER_OUTSIDE_LIMIT);
  }

  // Do the math to convert to Axis1 and Axis2
  A1Steps = ASteps + BSteps;
  A2Steps = ASteps - BSteps;
    
  // Check for too-fast step request (>25KHz)
  // First get absolute value of steps, then check if it's asking for >25KHz
  if (A1Steps > 0) 
  {
    Steps = A1Steps;
  }
  else 
  {
    Steps = -A1Steps;
  }
  // Limit each parameter to just 3 bytes
  if (Duration > 0xFFFFFF) 
  {
    printf((far rom char *)"!0 Err: <move_duration> larger than 16777215 ms.\n\r");
    return;
  }
  if (Steps > 0xFFFFFF) 
  {
    printf((far rom char *)"!0 Err: <axis1> larger than 16777215 steps.\n\r");
    return;
  }
  // Check for too fast
  if ((Steps/Duration) > HIGH_ISR_TICKS_PER_MS) 
  {
    printf((far rom char *)"!0 Err: <axis1> step rate > 25K steps/second.\n\r");
    return;
  }
  // And check for too slow
  if ((Duration/1311) >= Steps && Steps != 0)
  {
    printf((far rom char *)"!0 Err: <axis1> step rate < 1.31Hz.\n\r");
    return;
  }

  if (A2Steps > 0) 
  {
    Steps = A2Steps;
  }
  else {
    Steps = -A2Steps;
  }    
  if (Steps > 0xFFFFFF) 
  {
    printf((far rom char *)"!0 Err: <axis2> larger than 16777215 steps.\n\r");
    return;
  }
  if ((Steps/Duration) > HIGH_ISR_TICKS_PER_MS) 
  {
    printf((far rom char *)"!0 Err: <axis2> step rate > 25K steps/second.\n\r");
    return;
  }
  if ((Duration/1311) >= Steps && Steps != 0) 
  {
    printf((far rom char *)"!0 Err: <axis2> step rate < 1.31Hz.\n\r");
    return;
  }

    // Bail if we got a conversion error
  if (error_byte)
  {
    return;
  }

  // If we get here, we know that step rate for both A1 and A2 is
  // between 25KHz and 1.31Hz which are the limits of what EBB can do.
  /// TODO: Make into 3rd axis?
  process_SM(Duration, A1Steps, A2Steps, 0);

  print_ack();
}

// Main stepper move function. This is the reason EBB exists.
// <Duration> is a 3 byte unsigned int, the number of mS that the move should take
// <A1Stp> and <A2Stp> are the Axis 1 and Axis 2 number of steps to take in
//  <Duration> mS, as 3 byte signed values, where the sign determines the motor
//  direction.
// This function waits until there is room in the 1-deep FIFO before placing
// the data in the FIFO. The ISR then sees this data when it is done with its
// current move, and starts this new move.
//
// In the future, making the FIFO more elements deep may be cool.
// 
void process_SM(UINT32 Duration, INT32 A1Stp, INT32 A2Stp, INT32 A3Stp)
{
  UINT32 temp = 0;
  UINT32 temp1 = 0;
  UINT32 temp2 = 0;
  UINT32 remainder = 0;
  /// TODO: Remove 'move' and just write directly to [FIFOIn] to save stack space
  MoveCommandType move;

  // Uncomment the following printf() for debugging
  //printf((far rom char *)"Duration=%lu SA1=%li SA2=%li\n\r",
  //        Duration,
  //        A1Stp,
  //        A2Stp
  //    );

  // Check for delay
  if (A1Stp == 0 && A2Stp == 0 && A3Stp == 0)
  {
    move.Command = COMMAND_DELAY;
    // This is OK because we only need to multiply the 3 byte Duration by
    // 25, so it fits in 4 bytes OK.
    move.DelayCounter = HIGH_ISR_TICKS_PER_MS * Duration;
  }
  else
  {
    move.DelayCounter = 0; // No delay for motor moves
    move.DirBits = 0;

/// TODO: Get this to work for 3BB as well
#if defined(BOARD_EBB)
    // Always enable both motors when we want to move them
    Enable1IO = ENABLE_MOTOR;
    Enable2IO = ENABLE_MOTOR;
    RCServoPowerIO = RCSERVO_POWER_ON;
    gRCServoPoweroffCounterMS = gRCServoPoweroffCounterReloadMS;
#endif
    
    // First, set the direction bits
    if (A1Stp < 0)
    {
      move.DirBits = move.DirBits | DIR1_BIT;
      A1Stp = -A1Stp;
    }
    if (A2Stp < 0)
    {
      move.DirBits = move.DirBits | DIR2_BIT;
      A2Stp = -A2Stp;
    }
    if (A3Stp < 0)
    {
      move.DirBits = move.DirBits | DIR3_BIT;
      A3Stp = -A3Stp;
    }

    // To compute StepAdd values from Duration.
    // A1Stp is from 0x000001 to 0xFFFFFF.
    // HIGH_ISR_TICKS_PER_MS = 25
    // Duration is from 0x000001 to 0xFFFFFF.
    // temp needs to be from 0x0001 to 0x7FFF.
    // Temp is added to accumulator every 25KHz. So slowest step rate
    // we can do is 1 step every 25KHz / 0x7FFF or 1 every 763mS. 
    // Fastest step rate is obviously 25KHz.
    // If A1Stp is 1, then duration must be 763 or less.
    // If A1Stp is 2, then duration must be 763 * 2 or less.
    // If A1Stp is 0xFFFFFF, then duration must be at least 671088.
        
    // First check for duration to large.
    //        if (A1Stp < (0xFFFFFF/763)) {
    //            if (duration > (A1Stp * 763)) {
    //                printf((far rom char *)"Major malfunction Axis1 duration too long : %lu\n\r", duration);
    //                temp = 0;
    //                A1Stp = 0;
    //            }
    //        }
        
    if (A1Stp < 0x1FFFF) 
    {
      temp1 = HIGH_ISR_TICKS_PER_MS * Duration;
      temp = (A1Stp << 15)/temp1;
      temp2 = (A1Stp << 15) % temp1;
      /* Because it takes us about 5ms extra time to do this division,
       * we only perform this extra step if our move is long enough to
       * warrant it. That way, for really short moves (where the extra
       * precision isn't necessary) we don't take up extra time. Without
       * this optimization, our minimum move time is 20ms. With it, it
       * drops down to about 15ms.
       */
      if (Duration > 30)
      {
        remainder = (temp2 << 16) / temp1;
      }
    }
    else 
    {
      temp = (((A1Stp/Duration) * (UINT32)0x8000)/(UINT32)HIGH_ISR_TICKS_PER_MS);
      remainder = 0;
    }
    if (temp > 0x8000) 
    {
      printf((far rom char *)"Major malfunction Axis1 StepCounter too high : %lu\n\r", temp);
      temp = 0x8000;
    }
    if (temp == 0 && A1Stp != 0) {
      printf((far rom char *)"Major malfunction Axis1 StepCounter zero\n\r");
      temp = 1;
    }
    if (Duration > 30)
    {
      temp = (temp << 16) + remainder;
    }
    else
    {
      temp = (temp << 16);
    }

    move.StepAdd[0] = temp;
    move.StepsCounter[0] = A1Stp;
    move.StepAddInc[0] = 0;

    if (A2Stp < 0x1FFFF) 
    {
      temp = (A2Stp << 15)/temp1;
      temp2 = (A2Stp << 15) % temp1; 
      if (Duration > 30)
      {
        remainder = (temp2 << 16) / temp1;
      }
    }
    else 
    {
      temp = (((A2Stp/Duration) * (UINT32)0x8000)/(UINT32)HIGH_ISR_TICKS_PER_MS);
      remainder = 0;
    }
    if (temp > 0x8000) 
    {
      printf((far rom char *)"Major malfunction Axis2 StepCounter too high : %lu\n\r", temp);
      temp = 0x8000;
    }
    if (temp == 0 && A2Stp != 0) 
    {
      printf((far rom char *)"Major malfunction Axis2 StepCounter zero\n\r");
      temp = 1;
    }
    if (Duration > 30)
    {
      temp = (temp << 16) + remainder;
    }
    else
    {
      temp = (temp << 16);
    }
        
    move.StepAdd[1] = temp;
    move.StepsCounter[1] = A2Stp;
    move.StepAddInc[1] = 0;
    move.Command = COMMAND_MOTOR_MOVE;
     
    if (A3Stp < 0x1FFFF) 
    {
      temp = (A3Stp << 15)/temp1;
      temp2 = (A3Stp << 15) % temp1; 
      if (Duration > 30)
      {
        remainder = (temp2 << 16) / temp1;
      }
    }
    else 
    {
      temp = (((A3Stp/Duration) * (UINT32)0x8000)/(UINT32)HIGH_ISR_TICKS_PER_MS);
      remainder = 0;
    }
    if (temp > 0x8000) 
    {
      printf((far rom char *)"Major malfunction Axis3 StepCounter too high : %lu\n\r", temp);
      temp = 0x8000;
    }
    if (temp == 0 && A3Stp != 0) 
    {
      printf((far rom char *)"Major malfunction Axis3 StepCounter zero\n\r");
      temp = 1;
    }
    if (Duration > 30)
    {
      temp = (temp << 16) + remainder;
    }
    else
    {
      temp = (temp << 16);
    }
        
    move.StepAdd[2] = temp;
    move.StepsCounter[2] = A3Stp;
    move.StepAddInc[2] = 0;
    move.Command = COMMAND_MOTOR_MOVE;
    
    /* For debugging step motion , uncomment the next line */

    //printf((far rom char *)"SA1=%lu SC1=%lu SA2=%lu SC2=%lu\n\r",
    //        move.StepAdd[0],
    //        move.StepsCounter[0],
    //        move.StepAdd[1],
    //        move.StepsCounter[1]
    //    );
  }

  // Spin here until there's space in the fifo
  WaitForRoomInFIFO();

  // Now, quick copy over the computed command data to the command fifo
  FIFO_Command[FIFOIn] = move.Command;

  FIFO_StepAdd0[FIFOIn] = move.StepAdd[0];
  FIFO_G5[FIFOIn].StepAddInc0 = move.StepAddInc[0];
  FIFO_G2[FIFOIn].StepsCounter0 = move.StepsCounter[0];

  FIFO_StepAdd1[FIFOIn] = move.StepAdd[1];
  FIFO_StepAddInc1[FIFOIn] = move.StepAddInc[1];
  FIFO_G3[FIFOIn].StepsCounter1 = move.StepsCounter[1];

  FIFO_G1[FIFOIn].StepAdd2 = move.StepAdd[2];
  FIFO_StepAddInc2[FIFOIn] = move.StepAddInc[2];
  FIFO_G4[FIFOIn].StepsCounter2 = move.StepsCounter[2];
  
  FIFO_DirBits[FIFOIn] = move.DirBits;
  
  fifo_Inc();

}

// Handle the EStop functionality: stop all motor motion in steppers and servo,
// print out the interrupted state (if printResult == TRUE), and start the 
// motor limp timer if it was non zero which will limp the 3 motors after
// the a delay.
void process_EStop(BOOL printResult)
{
  UINT8 command_interrupted = 0;
  UINT32 remaining_steps1 = 0;
  UINT32 remaining_steps2 = 0;
  UINT32 fifo_steps1 = 0;
  UINT32 fifo_steps2 = 0;
#if 0
  // If there is a command waiting in the FIFO and it is a move command
  // or the current command is a move command, then remember that for later.
  if (
      (!FIFOEmpty && FIFO_Command[0] == COMMAND_MOTOR_MOVE)
      || 
      CurrentCommand_Command == COMMAND_MOTOR_MOVE
  )
  {
    command_interrupted = 1;
  }

  // If the FIFO has a move command in it, remove it.
  if (FIFO_Command[0] == COMMAND_MOTOR_MOVE)
  {
    FIFO_Command[0] = COMMAND_NONE;
    fifo_steps1 = FIFO_StepsCounter[0][0];
    fifo_steps2 = FIFO_StepsCounter[1][0];
    FIFO_StepsCounter[0][0] = 0;
    FIFO_StepsCounter[1][0] = 0;
    FIFO_StepAddInc[0][0] = 0;
    FIFO_StepAddInc[1][0] = 0;
    FIFOEmpty = TRUE;
  }

  // If the current command is a move command, then stop the move.
  if (CurrentCommand_Command == COMMAND_MOTOR_MOVE)
  {
    CurrentCommand_Command = COMMAND_NONE;
    remaining_steps1 = CurrentCommand_StepsCounter[0];
    remaining_steps2 = CurrentCommand_StepsCounter[1];
    CurrentCommand_StepsCounter[0] = 0;
    CurrentCommand_StepsCounter[1] = 0;
    CurrentCommand_StepAddInc[0] = 0;
    CurrentCommand_StepAddInc[1] = 0;
  }
  
  // Stop the servo's motion

  // Check to see if there is a limp delay set. If so, start the countdown
  // timer that will limp the 3 motors after that delay.
///  if (EStopLimpDelayMS)
///  {
///    EStopLimpDelayTimerMS = EStopLimpDelayMS;
///  }
#endif
  if (printResult)
  {
    printf((far rom char *)"%d,%lu,%lu,%lu,%lu\n\r", 
            command_interrupted,
            fifo_steps1,
            fifo_steps2,
            remaining_steps1,
            remaining_steps2
        );
    print_ack();
  }
}

// E-Stop
// Usage: ES<CR>
// Returns: <command_interrupted>,<fifo_steps1>,<fifo_steps2>,<steps_remaining1>,<steps_remaining2><CR>OK<CR>
// This command will abort any in-progress motor move (SM) command.
// It will also clear out any pending command(s) in the FIFO.
// <command_interrupted> = 0 if no FIFO or in-progress move commands were interrupted,
//                         1 if a motor move command was in progress or in the FIFO
// <fifo_steps1> and <fifo_steps1> = 24 bit unsigned integers with the number of steps
//                         in any SM command sitting in the fifo for axis1 and axis2.
// <steps_remaining1> and <steps_remaining2> = 24 bit unsigned integers with the number of
//                         steps left in the currently executing SM command (if any) for
//                         axis1 and axis2.
// It will return 0,0,0,0,0 if no SM command was executing at the time, and no SM
// command was in the FIFO.
void parseESCommand(void)
{
  process_EStop(TRUE);
}


// Do the work of the QM command so we can use this same code for QM and
// for QG commands.
UINT8 process_QM(void)
{
  UINT8 CommandExecuting = 0;
  UINT8 Motor1Running = 0;
  UINT8 Motor2Running = 0;
  UINT8 FIFOStatus = 0;

  // Need to turn off high priority interrupts breifly here to read out value that ISR uses
  INTCONbits.GIEH = 0;  // Turn high priority interrupts off

  // Create our output values to print back to the PC
  if (FIFODepth) 
  {
    CommandExecuting = 1;
  }
  if (FIFODepth > 1)
  {
    FIFOStatus = 1;
  }
  if (CommandExecuting && FIFO_G2[FIFOOut].StepsCounter0 != 0) 
  {
    Motor1Running = 1;
  }
  if (CommandExecuting && FIFO_G3[FIFOOut].StepsCounter1 != 0) 
  {
    Motor2Running = 1;
  }

  // Re-enable interrupts
  INTCONbits.GIEH = 1;  // Turn high priority interrupts on
    
  return ((CommandExecuting << 3) | (Motor1Running << 2) | (Motor2Running << 1) | FIFOStatus);
}

// QM command
// For Query Motor - returns the current status of each motor
// QM takes no parameters, so usage is just QM<CR>
// QM returns:
// QM,<CommandExecutingStatus>,<Motor1Satus>,<Motor2Status><CR>
// where:
//   <CommandExecutingStatus>: 0 if no 'motion command' is executing, > 0 if some 'motion command' is executing
//   <Motor1Status>: 0 if motor 1 is idle, 1 if motor is moving
//   <Motor2Status>: 0 if motor 2 is idle, 1 if motor is moving
//
// As of version 2.4.4, there is now a fourth parameter at the end of the reply packet.
// QM,<CommandExecutingStatus>,<Motor1Satus>,<Motor2Status>,<FIFOStatus><CR>
// Where <FIFOStatus> is either 1 (if there are any commands in the FIFO) or 0 (if the FIFO is empty)
void parseQMCommand(void)
{
  UINT8 CommandExecuting = 0;
  UINT8 Motor1Running = 0;
  UINT8 Motor2Running = 0;
  UINT8 FIFOStatus = 0;
  UINT8 result = process_QM();

  if (result & 0x01)
  {
    FIFOStatus = 1;
  }
  if (result & 0x02)
  {
    Motor2Running = 1;
  }
  if (result & 0x04)
  {
    Motor1Running = 1;
  }
  if (result & 0x08)
  {
    CommandExecuting = 1;
  }

  printf((far ROM char *)"QM,%i,%i,%i,%i\n\r", CommandExecuting, Motor1Running, Motor2Running, FIFOStatus);
}

// QS command
// For Query Step position - returns the current x and y global step positions
// QS takes no parameters, so usage is just CS<CR>
// QS returns:
// QS,<global_step1_position>,<global_step2_position><CR>
// where:
//   <global_step1_position>: signed 32 bit value, current global motor 1 step position
//   <global_step2_position>: signed 32 bit value, current global motor 2 step position
void parseQSCommand(void)
{
  INT32 step1, step2;

  // Need to turn off high priority interrupts breifly here to read out value that ISR uses
  INTCONbits.GIEH = 0;  // Turn high priority interrupts off

  // Make a local copy of the things we care about
  step1 = globalStepCounter1;
  step2 = globalStepCounter2;

  // Re-enable interrupts
  INTCONbits.GIEH = 1;  // Turn high priority interrupts on

  printf((far ROM char *)"%li,%li\n\r", step1, step2);
  print_ack();
}

// Perform the actual clearing of the step counters (used from several places)
void clear_StepCounters(void)
{
  // Need to turn off high priority interrupts breifly here to read out value that ISR uses
  INTCONbits.GIEH = 0;  // Turn high priority interrupts off

  // Make a local copy of the things we care about
  globalStepCounter1 = 0;
  globalStepCounter2 = 0;
  globalStepCounter3 = 0;

  // Re-enable interrupts
  INTCONbits.GIEH = 1;  // Turn high priority interrupts on
}

// CS command
// For Clear Stepper position - zeros out both step1 and step2 global positions
// CS takes no parameters, so usage is just CS<CR>
// QS returns:
// OK<CR>
void parseCSCommand(void)
{
  clear_StepCounters();
  
  print_ack();
}
