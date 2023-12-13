/***********************************************************************************************************************
*                                                                                                                      *
* specbridge                                                                                                           *
*                                                                                                                      *
* Copyright (c) 2012-2023 Andrew D. Zonenberg                                                                          *
* All rights reserved.                                                                                                 *
*                                                                                                                      *
* Redistribution and use in source and binary forms, with or without modification, are permitted provided that the     *
* following conditions are met:                                                                                        *
*                                                                                                                      *
*    * Redistributions of source code must retain the above copyright notice, this list of conditions, and the         *
*      following disclaimer.                                                                                           *
*                                                                                                                      *
*    * Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the       *
*      following disclaimer in the documentation and/or other materials provided with the distribution.                *
*                                                                                                                      *
*    * Neither the name of the author nor the names of any contributors may be used to endorse or promote products     *
*      derived from this software without specific prior written permission.                                           *
*                                                                                                                      *
* THIS SOFTWARE IS PROVIDED BY THE AUTHORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED   *
* TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL *
* THE AUTHORS BE HELD LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES        *
* (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR       *
* BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT *
* (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE       *
* POSSIBILITY OF SUCH DAMAGE.                                                                                          *
*                                                                                                                      *
***********************************************************************************************************************/

/**
	@file
	@author Andrew D. Zonenberg
	@brief Waveform data thread (data plane traffic only, no control plane SCPI)
 */
#include "specbridge.h"
#include <string.h>

using namespace std;

volatile bool g_waveformThreadQuit = false;

void WaveformServerThread()
{
#ifdef __linux__
	pthread_setname_np(pthread_self(), "WaveformThread");
#endif

	Socket client = g_dataSocket.Accept();
	LogVerbose("Client connected to data plane socket\n");

	if(!client.IsValid())
		return;
	if(!client.DisableNagle())
		LogWarning("Failed to disable Nagle on socket, performance may be poor\n");

	const int framesize = 3699;
	uint16_t* framePixels = new uint16_t[framesize];
	float* frameFlattened = new float[g_numPixels];

	while(!g_waveformThreadQuit)
	{
		//wait if trigger not armed
		if(!g_triggerArmed)
		{
			this_thread::sleep_for(chrono::microseconds(1000));
			continue;
		}

		//Acquire data
		{
			lock_guard<mutex> lock(g_mutex);

			//Trigger an acquisition
			int err;
			if(0 != (err = triggerAcquisition(&g_hDevice)))
				LogError("failed to trigger acquisition, code %d\n", err);

			//Get the frame data
			if(0 != (err = getFrame(framePixels, 0xffff, &g_hDevice)))
			{
				LogError("failed to get frame, code %d\n", err);
				break;
			}

			if(g_triggerOneShot)
				g_triggerArmed = false;
		}

		//Frame data seems to be *mirrored* - shortest wavelengths at right... But we'll fix that clientside.
		for(int i=0; i<g_numPixels; i++)
			frameFlattened[i] = framePixels[i+32];

		//Send the flattened data to the client
		if(!client.SendLooped((uint8_t*)frameFlattened, g_numPixels * sizeof(float)))
			break;
	}

	LogDebug("Client disconnected from data plane socket\n");

	//Clean up
	delete[] framePixels;
	delete[] frameFlattened;
}
