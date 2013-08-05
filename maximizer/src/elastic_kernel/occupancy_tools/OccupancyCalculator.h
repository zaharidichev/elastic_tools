/**
 * OccupancyCalculator.hpp
 *
 *  Created on: Jun 20, 2013
 *      Author: Zahari Dichev <zaharidichev@gmail.com>
 */

#ifndef OCCUPANCYCALCULATOR_HPP_
#define OCCUPANCYCALCULATOR_HPP_
#include "OccupancyData.h"
#include "OccupancyLimits.h"
#include <cuda_runtime.h>
#include <iostream>
#include <cmath>
#include "../AbstractElasticKernel.hpp"
#include <boost/shared_ptr.hpp>

struct OccupancyInformation {
	int optimalThreadBlockSize;
	double respectiveSMOccupancy;
};

inline cudaDeviceProp getGPUProperties() {
	cudaDeviceProp props;
	cudaGetDeviceProperties(&props, 0);
	return props;
}

// WORKS FINE
inline size_t getMaxResidentBlocksPerSM(const cudaDeviceProp &deviceProps, const cudaFuncAttributes &kernelProps, size_t blockSize) {

	size_t hardwareLimit = getHardwareLimit(deviceProps, blockSize);
	size_t sMemLimit = getSharedMemLimit(deviceProps, kernelProps);
	size_t registerLimit = getRegisterLimit(deviceProps, kernelProps, blockSize);

	//std::cout << hardwareLimit << " " << sMemLimit << " " << registerLimit << std::endl;

	return min3(hardwareLimit, sMemLimit, registerLimit);
}

// WORKS FINE
inline size_t getNumRegistersPerBlock(const cudaDeviceProp &deviceProps, const cudaFuncAttributes &kernelProps, size_t blockSize) {

	size_t numerWarpsNeeded = (blockSize + (deviceProps.warpSize - 1)) / deviceProps.warpSize; // we need to devide and round UP to warpsize
	numerWarpsNeeded = ceilTo(numerWarpsNeeded, getWarpAllocationGranularity(deviceProps)); // again CEIL up to the war allocation granularity, depending on architecture;

	return kernelProps.numRegs * deviceProps.warpSize * numerWarpsNeeded;
}

//WORKS FINE
inline BlockUsage getBlockUsageStats(const cudaDeviceProp &deviceProps, const cudaFuncAttributes &kernelProps, size_t blockSize) {

	BlockUsage usage;
	size_t numThreads = blockSize;
	size_t blocksPerSM = getMaxResidentBlocksPerSM(deviceProps, kernelProps, blockSize);
	size_t numRegisters = getNumRegistersPerBlock(deviceProps, kernelProps, blockSize);
	size_t sharedMemory = getSharedMemNeeded(kernelProps, deviceProps);

	return BlockUsage(sharedMemory, numThreads, numRegisters, blocksPerSM);
}
inline cudaDeviceProp getDeviceProps() {
	cudaDeviceProp props;
	cudaGetDeviceProperties(&props, 0);
	return props;
}

inline void reduceBlocksToFitOnGPU(size_t usagePerBlock, size_t limitPerGPU, LaunchParameters &params) {
	// calcualte how much is the usage for the whole card...

	size_t currentUsage = params.getBlocksPerGrid() * usagePerBlock;
	//std::cout << "USAGE per block" << usagePerBlock << std::endl;

	//std::cout << "USAGE " << currentUsage << std::endl;
	//std::cout << "LIMIT " << limitPerGPU << std::endl;

	if (currentUsage > limitPerGPU) {
		// if we exceed the limit.. we need to decrement

		int deficit = currentUsage - limitPerGPU; //  calculatye the total deficit
		//std::cout << "Deficit " << deficit << std::endl;

		size_t decrement = ceil((double) deficit / usagePerBlock); // calcualte how many blocks we need to trim in order to fit

		size_t decreasedBlocks = params.getBlocksPerGrid() - decrement;

		params.setBlocks(decreasedBlocks);
	}
}

inline LaunchParameters limitUsage(const cudaDeviceProp &deviceProps, const cudaFuncAttributes &kernelProps, LaunchParameters lParams, KernelLimits limits) {
	//std::cout << "REGS " << kernelProps.numRegs << std::endl;
	// we get the occupancy information for the particualr block
	BlockUsage usage = getBlockUsageStats(deviceProps, kernelProps, lParams.getThreadsPerBlock());
	//std::cout << usage << std::endl;
	// we calcualte the maximum number of resident blocks of this size on the GPU
	size_t maximumResidentBLocks = usage.getNumBlocksPerSM() * deviceProps.multiProcessorCount;

	// constructing physical configuration... based on calcualted number of blocks :)
	size_t blocksPhysical = min3(lParams.getBlocksPerGrid(), maximumResidentBLocks, limits.getNumBlocks());
	size_t threadsPhysical = lParams.getThreadsPerBlock();
	LaunchParameters result = LaunchParameters(threadsPhysical, blocksPhysical);

	// now we need to further limit this physical  configuration, which of course is pain

	reduceBlocksToFitOnGPU(usage.getSharedMem(), limits.getSharedMem(), result);
	reduceBlocksToFitOnGPU(usage.getNumThreads(), limits.getNumThreads(), result);
	reduceBlocksToFitOnGPU(usage.getNumRegisters(), limits.getNumRegisters(), result);

	return result;
}

inline LaunchParameters limitKernel(boost::shared_ptr<AbstractElasticKernel> kernel, KernelLimits limits) {
	LaunchParameters params = kernel.get()->getLaunchParams();
	cudaFuncAttributes attrs = kernel.get()->getKernelProperties();
	cudaDeviceProp props = getDeviceProps();
	return limitUsage(props, attrs, params, limits);

}

inline double getOccupancyForKernel(boost::shared_ptr<AbstractElasticKernel> kernel) {
	cudaDeviceProp gpuConfiguration = getGPUProperties();
	size_t max_occupancy = gpuConfiguration.maxThreadsPerMultiProcessor;
	size_t threadNum = min3(kernel.get()->getKernelProperties().maxThreadsPerBlock, gpuConfiguration.maxThreadsPerMultiProcessor,
			kernel.get()->getLaunchParams().getThreadsPerBlock());

	size_t maxBlocksPerSm = getMaxResidentBlocksPerSM(gpuConfiguration, kernel.get()->getKernelProperties(), threadNum);
	size_t occupancy = threadNum * maxBlocksPerSm;

	return (double) occupancy / max_occupancy;

}

inline double getMemoryOccupancyForKernel(boost::shared_ptr<AbstractElasticKernel> kernel) {
	cudaDeviceProp deviceProps = getGPUConfiguration();
	double totalGPUmem = (double) deviceProps.totalGlobalMem;
	double result = (double) kernel.get()->getMemoryConsumption() / totalGPUmem;

	return result;
}

inline size_t getOptimalBlockSize(boost::shared_ptr<AbstractElasticKernel> kernel) {

	cudaDeviceProp gpuConfiguration = getGPUProperties();
	size_t max_occupancy = gpuConfiguration.maxThreadsPerMultiProcessor;
	size_t largestThrNum = min2(kernel.get()->getKernelProperties().maxThreadsPerBlock, gpuConfiguration.maxThreadsPerMultiProcessor);

	size_t threadGranularity = gpuConfiguration.warpSize;

	size_t maxBLockSize = 0;
	size_t highestOcc = 0;

	for (size_t blocksize = largestThrNum; blocksize != 0; blocksize -= threadGranularity) {

		size_t maxBlocksPerSm = getMaxResidentBlocksPerSM(gpuConfiguration, kernel.get()->getKernelProperties(), blocksize);
		size_t occupancy = blocksize * maxBlocksPerSm;

		//std::cout << "Trying blocksize "  << blocksize << " " << occupancy  << " " << highestOcc  <<  " " << (double)occupancy / max_occupancy << std::endl;

		if (occupancy > highestOcc) {
			maxBLockSize = blocksize;
			highestOcc = occupancy;
		}

		// early out, can't do better
		if (highestOcc == max_occupancy)
			break;
	}
	//printf("Blocksize %d\n", highestOcc);

	return maxBLockSize;

}

#endif /* OCCUPANCYCALCULATOR_HPP_ */
