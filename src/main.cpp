/*================================================================================
This software is open source software available under the BSD-3 license.

Copyright (c) 2017, Los Alamos National Security, LLC.
All rights reserved.

Authors:
 - Pascal Grosset
 - Jesus Pulido
 - Chris Biwer
================================================================================*/

#include <iostream>
#include <sstream>
#include <vector>

#include <mpi.h>

// Helper Functions
#include "json.hpp"
#include "timer.hpp"
#include "log.hpp"
#include "memory.hpp"
#include "utils.hpp"

// Interfaces
#include "dataLoaderInterface.hpp"
#include "compressorInterface.hpp"
#include "metricInterface.hpp"

// Factories
#include "compressorFactory.hpp"
#include "metricFactory.hpp"

// Readers
#include "BinaryDataLoader.hpp"
#include "HACCDataLoader.hpp"
#ifdef CBENCH_HAS_NYX
	#include "NYXDataLoader.hpp"
#endif




// Function to validate whatever could be wrong with the input
int validateInput(int argc, char *argv[], int myRank, int numRanks)
{
	// Check if we have the right number of arguments
	if (argc < 2)
	{
		if (myRank == 0)
		{
			std::cerr << "Input argument needed. Run as: ../inputs/all.json" << std::endl;
			std::cerr << "Read arguments: " << argc << std::endl;
		}

		return 0;
	}

	// Check if input file provided exists
	if ( !fileExisits(argv[1]) )
	{
		if (myRank == 0)
			std::cerr << "Could not find input JSON file " << argv[1] << "." << std::endl;

		return 0;
	}



	// Validate JSON file
	nlohmann::json jsonInput;
	std::ifstream jsonFile(argv[1]);

	try
    {
		jsonFile >> jsonInput;
	}
	catch (nlohmann::json::parse_error& e)
    {
       	if (myRank == 0)
        	std::cerr << "Input JSON file " << argv[1] << " is invalid!\n" 
        			  << e.what() << "\n"
        			  << "Validate your JSON file using e.g. https://jsonformatter.curiousconcept.com/ " << std::endl;
        return 0;
    }



    // Check if powers of 2 number of ranks
    if (jsonInput["output"].find("output-decompressed") != jsonInput["output"].end())
		if ( !isPowerOfTwo(numRanks) )
		{
			if (myRank == 0)
				std::cerr << "Please run with powers of 2 number of ranks. e.g. 4, 8, 16, 32, ... when writing out HACC files." << std::endl;

			return 0;
		}

	return 1;
}



int main(int argc, char *argv[])
{
	//
	// init MPI
	int myRank, numRanks, threadSupport;
	MPI_Init_thread(NULL, NULL, MPI_THREAD_MULTIPLE, &threadSupport);
	MPI_Comm_size(MPI_COMM_WORLD, &numRanks);
	MPI_Comm_rank(MPI_COMM_WORLD, &myRank);


	//
	// Validate input params
	if ( !validateInput(argc, argv, myRank, numRanks) )
	{
		MPI_Finalize();
        return 0;
	}
	

	//
	// Load input

	// Pass JSON file to json parser and 
	nlohmann::json jsonInput;
	std::ifstream jsonFile(argv[1]);
	jsonFile >> jsonInput;

	// Load in the parameters
	std::string inputFileType = jsonInput["input"]["filetype"];
	std::string inputFile = jsonInput["input"]["filename"];

	std::string outputLogFile = jsonInput["output"]["logfname"];
	outputLogFile += "_" + std::to_string(myRank);

	std::string metricsFile = jsonInput["output"]["metricsfname"];

	std::vector< std::string > params;
	for (int j = 0; j < jsonInput["input"]["scalars"].size(); j++)
		params.push_back( jsonInput["input"]["scalars"][j] );

	std::vector< std::string > compressors;
	for (int j = 0; j < jsonInput["compressors"].size(); j++)
		compressors.push_back( jsonInput["compressors"][j] );

	std::vector< std::string > metrics;
	for (int j = 0; j < jsonInput["metrics"].size(); j++)
		metrics.push_back(jsonInput["metrics"][j]);

	bool writeData = false;
	std::string outputFile = "";
	if (jsonInput["output"].find("output-decompressed") != jsonInput["output"].end())
	{
		outputFile = extractFileName(inputFile) + "__";
		writeData = true;
	}


	//
	// For humans; all seems valid, let's start ...
	if (myRank == 0)
		std::cout << "Starting ... \nLook at the log for progress update ... \n" << std::endl;



	//
	// Create log and metrics files
	Timer overallClock;
	std::stringstream debuglog, metricsInfo, csvOutput;
	writeLog(outputLogFile, debuglog.str());

	csvOutput << "Compressor_field" << ", ";
	for (int m = 0; m < metrics.size(); ++m)
		csvOutput << metrics[m] << ", ";

	csvOutput << "Max Compression Throughput, Max DeCompression Throughput, ";
	csvOutput << "Min Compression Throughput, Min DeCompression Throughput, Compression Ratio" << std::endl;
	metricsInfo << "Input file: " << inputFile << std::endl;

	overallClock.start();



	//
	// Open file
	DataLoaderInterface *ioMgr;

	if (inputFileType == "Binary")
		ioMgr = new BinaryDataLoader();
	else if (inputFileType == "HACC")
		ioMgr = new HACCDataLoader();
  #ifdef CBENCH_HAS_NYX
	else if (inputFileType == "NYX")
		ioMgr = new NYXDataLoader();
  #endif
	else
	{
		// Exit if no input file is provided
		if (myRank == 0)
			std::cout << "Unsupported format " << inputFileType << "!!!" << std::endl;

		MPI_Finalize();
		return 0;
	}


	// Check if the datainfo field exist for a dataset
	if (jsonInput["input"].find("datainfo") != jsonInput["input"].end())
	{
		// insert datainfo into loader parameter list
		for (auto it = jsonInput["input"]["datainfo"].begin(); it != jsonInput["input"]["datainfo"].end(); it++)
			ioMgr->loaderParams[it.key()] = strConvert::toStr(it.value());
	}

	ioMgr->init(inputFile, MPI_COMM_WORLD);
	ioMgr->setSave(writeData);
	

	// Save parameters of input file to facilitate rewrite
	if (writeData)
		ioMgr->saveInputFileParameters();


	//
	// Cycle through compressors and parameters
	CompressorInterface *compressorMgr;
	MetricInterface *metricsMgr;

	for (int c = 0; c < compressors.size(); ++c)
	{	
		// initialize compressor
		compressorMgr = CompressorFactory::createCompressor(compressors[c]);
		if (compressorMgr == NULL)
		{
			if (myRank == 0)
				std::cout << "Unsupported compressor: " << compressors[c] << " ... Skipping!" << std::endl;
			continue;
		}

		// Check if the parameters field exist
		if (jsonInput["input"].find("parameters") != jsonInput["input"].end())
		{
			// insert parameter into compressor parameter list
			for (auto it=jsonInput["input"]["parameters"].begin(); it != jsonInput["input"]["parameters"].end(); it++)
				compressorMgr->compressorParameters[it.key()] = strConvert::toStr(it.value());	
		}


		// log
		compressorMgr->init();
		metricsInfo << "\n---------------------------------------" << std::endl;
		metricsInfo << "Compressor: " << compressorMgr->getCompressorName() << std::endl;

		debuglog << "===============================================" << std::endl;
		debuglog << "Compressor: " << compressorMgr->getCompressorName() << std::endl;		
		
		
		// Cycle through params
		for (int i=0; i<params.size(); i++)
		{
			Timer compressClock, decompressClock;
			Memory memLoad;

			memLoad.start();

			// Check if parameter is valid before proceding
			if ( !ioMgr->loadData(params[i]) )
			{
				memLoad.stop();
				continue;
			}

			
			// log stuff
			debuglog << ioMgr->getDataInfo();
			debuglog << ioMgr->getLog();
			appendLog(outputLogFile, debuglog);
			csvOutput << compressorMgr->getCompressorName() << "_" << params[i] << ", ";

			MPI_Barrier(MPI_COMM_WORLD);
			

			//
			// compress
			void * cdata = NULL;

			compressClock.start();
			compressorMgr->compress(ioMgr->data, cdata, ioMgr->getType(), ioMgr->getTypeSize(), ioMgr->getSizePerDim());
			compressClock.stop();


			//
			// decompress
			void * decompdata = NULL;

			decompressClock.start();
			compressorMgr->decompress(cdata, decompdata, ioMgr->getType(), ioMgr->getTypeSize(), ioMgr->getSizePerDim() );
			decompressClock.stop();


			// Get compression ratio
			unsigned long totalCompressedSize;
			unsigned long compressedSize = (unsigned long)compressorMgr->getCompressedSize();
			MPI_Allreduce(&compressedSize, &totalCompressedSize, 1, MPI_UNSIGNED_LONG, MPI_SUM, MPI_COMM_WORLD);
			
			unsigned long totalUnCompressedSize;
			unsigned long unCompressedSize = ioMgr->getTypeSize() * ioMgr->getNumElements();
			MPI_Allreduce(&unCompressedSize, &totalUnCompressedSize, 1, MPI_UNSIGNED_LONG, MPI_SUM, MPI_COMM_WORLD);


			debuglog << "\n\ncompressedSize: " << compressedSize << ", totalCompressedSize: " << totalCompressedSize << std::endl;
			debuglog << "unCompressedSize: " << unCompressedSize << ", totalUnCompressedSize: " << totalUnCompressedSize << std::endl;
			debuglog << "Compression ratio: " << totalUnCompressedSize/(float)totalCompressedSize << std::endl;

			writeLogApp(outputLogFile, compressorMgr->getLog());
			compressorMgr->clearLog();


			//
			// metrics
			debuglog << "\n----- " << params[i] << " error metrics ----- " << std::endl;
			metricsInfo << "\nField: " << params[i] << std::endl;
			for (int m = 0; m < metrics.size(); ++m)
			{
				metricsMgr = MetricsFactory::createMetric(metrics[m]);
				if (metricsMgr == NULL)
				{
					if (myRank == 0)
						std::cout << "Unsupported metric: " << metrics[m] << " ... Skipping!" << std::endl;
					continue;
				}


				metricsMgr->init(MPI_COMM_WORLD);
				metricsMgr->execute(ioMgr->data, decompdata, ioMgr->getNumElements());
				debuglog << metricsMgr->getLog();
				metricsInfo << metricsMgr->getLog();
				csvOutput << metricsMgr->getGlobalValue() << ", ";

				metricsMgr->close();
			}
			debuglog << "-----------------------------\n";
			debuglog << "\nMemory in use: " << memLoad.getMemoryInUseInMB() << " MB" << std::endl;



			//
			// Metrics Computation
			double compress_time = compressClock.getDuration();
			double decompress_time = decompressClock.getDuration();

			double compress_throughput = ((double)(ioMgr->getNumElements() * ioMgr->getTypeSize()) / (1024.0*1024.0) )/ compress_time;
			double decompress_throughput = ((double)(ioMgr->getNumElements() * ioMgr->getTypeSize()) / (1024.0*1024.0) )/ decompress_time;


			double max_compress_throughput = 0;
			double max_decompress_throughput = 0;
			double min_compress_throughput = 0;
			double min_decompress_throughput = 0;

			MPI_Reduce(&compress_throughput, &max_compress_throughput, 1, MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_WORLD);
			MPI_Reduce(&decompress_throughput, &max_decompress_throughput, 1, MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_WORLD);

			MPI_Reduce(&compress_throughput, &min_compress_throughput, 1, MPI_DOUBLE, MPI_MIN, 0, MPI_COMM_WORLD);
			MPI_Reduce(&decompress_throughput, &min_decompress_throughput, 1, MPI_DOUBLE, MPI_MIN, 0, MPI_COMM_WORLD);
		
			
			if (writeData)
			{
				ioMgr->saveCompData(params[i], decompdata);
				debuglog << ioMgr->getLog();
			}

			

			//
			// deallocate
			std::free(decompdata);
			

			ioMgr->close();
			memLoad.stop();


			//
			// log stuff
			debuglog << "\nCompress time: " << compress_time << std::endl;
			debuglog << "Decompress time: " << decompress_time << std::endl;
			debuglog << "\nMemory leaked: " << memLoad.getMemorySizeInMB() << " MB" << std::endl;
			debuglog << "........................................." << std::endl << std::endl;

			appendLog(outputLogFile, debuglog);
		
			if (myRank == 0)
			{
				metricsInfo << "Max Compression Throughput: " << max_compress_throughput << " Mbytes/s" << std::endl;
				metricsInfo << "Max DeCompression Throughput: " << max_decompress_throughput << " Mbytes/s" << std::endl;
				metricsInfo << "Min Compression Throughput: " << min_compress_throughput << " Mbytes/s" << std::endl;
				metricsInfo << "Min DeCompression Throughput: " << min_decompress_throughput << " Mbytes/s" << std::endl;
				metricsInfo << "Compression ratio: " << totalUnCompressedSize/(float)totalCompressedSize << std::endl;
				csvOutput << max_compress_throughput << ", " << max_decompress_throughput << ", " 
						  << min_compress_throughput << ", " << min_decompress_throughput << ", " 
						  << totalUnCompressedSize/(float)totalCompressedSize << "\n";

				writeFile(metricsFile, metricsInfo.str());
				writeFile(metricsFile + ".csv", csvOutput.str());
			}
			
			
			MPI_Barrier(MPI_COMM_WORLD);
		}
		
		
		
		if (writeData)
		{
			Timer clockWrite;
			clockWrite.start();

			// Pass data that was not compressed
			for (int i=0; i<ioMgr->inOutData.size(); i++)
			{
				if ( !ioMgr->inOutData[i].doWrite )
				{
					ioMgr->loadData(ioMgr->inOutData[i].name);
					ioMgr->saveCompData(ioMgr->inOutData[i].name, ioMgr->data);
					ioMgr->close();
				}
			}

			ioMgr->writeData(outputFile + compressorMgr->getCompressorName());

			clockWrite.stop();

			debuglog << ioMgr->getLog();
			debuglog << "Write output took: " <<  clockWrite.getDuration() << " s " << std::endl;
			appendLog(outputLogFile, debuglog );
		}
		
		compressorMgr->close();
	}

	overallClock.stop();
	debuglog << "\nTotal run time: " <<  overallClock.getDuration() << " s " << std::endl;
	appendLog(outputLogFile, debuglog );

	// For humans
	if (myRank == 0)
		std::cout << "\nThat's all folks!" << std::endl;

	MPI_Finalize();

	return 0;
}


/*
Run:
mpirun -np 2 CBench ../inputs/HACC_all.json
mpirun -np 8 ./CBench ../inputs/HACC_write_bigcrunch.json
*/
