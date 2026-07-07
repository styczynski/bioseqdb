#ifndef TIMER_H
#define TIMER_H

#include<string.h>
#include<stdlib.h>
#include<assert.h>
#include<sys/time.h>

class Timer
{
    private:
        struct timeval startTime;
        struct timeval stopTime;
        double elapsedTime;
        std::string name;

    public:
        Timer(std::string n) { name = n; elapsedTime = 0.0;}
	Timer() { name = ""; elapsedTime = 0.0;}
        void Clear() { elapsedTime = 0.0; }
        void Start() { gettimeofday(&(startTime), NULL); }
        void Restart()
        {
            elapsedTime = 0.0;
            gettimeofday(&(startTime), NULL);
        }

        void Pause()
        {
            gettimeofday(&(stopTime), NULL);

            elapsedTime +=  ( (stopTime).tv_sec  - (startTime).tv_sec) * 1000.0;      // sec to ms
            elapsedTime += ( (stopTime).tv_usec - (startTime).tv_usec) / 1000.0;   // us to ms
        }

        void Stop()
        {
            gettimeofday(&(stopTime), NULL);

            elapsedTime =  ( (stopTime).tv_sec  - (startTime).tv_sec) * 1000.0;      // sec to ms
            elapsedTime += ( (stopTime).tv_usec - (startTime).tv_usec) / 1000.0;   // us to ms
        }

        void Print()
        {
            std::cout << name << " : " <<  elapsedTime << " msec"   << std::endl;
        }

        double GetTime() { return elapsedTime;}

};


#endif
