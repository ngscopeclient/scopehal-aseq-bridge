###############################################################################
#C++ compilation
add_executable(specbridge
	AseqSCPIServer.cpp
	WaveformServerThread.cpp
	main.cpp
)

###############################################################################
#Linker settings
target_link_libraries(specbridge
	xptools
	log
	scpi-server-tools
	spectrometer
	)

