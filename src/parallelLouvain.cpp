/*
 * parallelLouvainTest.cpp
 *
 *  Created on: Apr 14, 2017
 *      Author: osu8229
 */
#include "parallelLouvain.h"

using namespace std;


// Assumes MPI::Init is already called in main
double louvain(Graph *G, unsigned long *communityInfo, double Lower,
				double threshold, double *totTime, int *numItr,
				int rank, int numProcs){

	double time1, time2, time;

	time1 = omp_get_wtime();

	unsigned long numOfVertices = G->numOfVertices;
	unsigned long numOfEdges = G->numOfEdges;
	unsigned long* vertexStartPointers = G->vertexStartPointers; // size of this is numOfVerticesOnProc + 1
	unsigned long* startVertices = G->startVertices;			   // size of this is numOfEdgesOnProc
	unsigned long* destinationVertices = G->destinationVertices; // size of this is numOfEdgesOnProc
	long* weights = G->weights;


	unsigned long numOfVerticesOnProc = getNumOfVerticesOnProc(numOfEdges, startVertices);
	long* vertexDegrees = (long *) malloc(numOfVerticesOnProc * sizeof(long));
	unsigned long totalEdgeWeightTwice;
	double constantForSecondTerm;
	double prevModularity = Lower;
	double currModularity = -1;
	double thresMod = threshold; //Input parameter
	int numItrs = 0;

	unsigned long* sizeOfCommunities = (unsigned long *) malloc(numOfVertices * sizeof(unsigned long));
	long* degreesOfCommunities = (long *) malloc(numOfVertices * sizeof(long));

	long* updateSizeOfCommunities = (long *) malloc(numOfVertices * sizeof(long));
	long* updateDegreesOfCommunities = (long *) malloc(numOfVertices * sizeof(long));

	double* eii = (double *) malloc(numOfVerticesOnProc * sizeof(double));

	int offsetToSend = 0;

	unsigned long numOfEdgesOnThisProc = getNumOfEdgesOnProc(numOfEdges, rank, numProcs);

//	if(rank == 0) cout<< "Before sumvertexdegrees"<<endl;

	sumVertexDegrees(vertexStartPointers,
					 startVertices,
					 weights,
					 vertexDegrees,
					 numOfVertices,
					 numOfVerticesOnProc,
					 numOfEdgesOnThisProc,
					 sizeOfCommunities,
					 degreesOfCommunities,
					 &offsetToSend);

	constantForSecondTerm = calculateConstantForSecondTerm(numOfVertices, degreesOfCommunities);
////	if(rank == 0) cout<< "After const"<<endl;
//
	unsigned long* pastCommunityAssignment = (unsigned long *) malloc(numOfVertices * sizeof(unsigned long));
	unsigned long* currCommunityAssignment = (unsigned long *) malloc(numOfVertices * sizeof(unsigned long));
	unsigned long* targetCommunityAssignment = (unsigned long *) malloc(numOfVerticesOnProc * sizeof(unsigned long));

	initialCommunityAssignment(pastCommunityAssignment,
							   currCommunityAssignment,
							   numOfVertices);



	// START MAXIMIZING MODULARITY
	while(true){
		numItrs++;

		#pragma omp parallel for
		for(unsigned long i = 0; i < numOfVertices; i++){
			updateSizeOfCommunities[i] = 0;
			updateDegreesOfCommunities[i] = 0;
		}

		#pragma omp parallel for
		for(unsigned long i = 0; i < numOfVerticesOnProc; i++){
			eii[i] = 0;
		}

		// FIND THE TARGET COMMUNITIES REGARDLESS OF WHETHER ALL DEST ARE ON THIS PROC
		#pragma omp parallel for
		for(unsigned long i = 0; i < numOfVerticesOnProc; i++){
			unsigned long currVertex = startVertices[vertexStartPointers[i]];

			unsigned long startPointer = vertexStartPointers[i];
			unsigned long endPointer = i+1 == numOfVerticesOnProc ? numOfEdgesOnThisProc : vertexStartPointers[i+1];
//			unsigned long endPointer = vertexStartPointers[i+1];

			long selfLoop = 0;

			map<unsigned long, long> communityDegreeMap;
			map<unsigned long, long>::iterator commDegreeMapIterator;

			if(startPointer != endPointer){
				communityDegreeMap[currCommunityAssignment[currVertex]] = 0;

				selfLoop = buildCommunityDegreeMap(startPointer, endPointer,
												   communityDegreeMap,
												   destinationVertices,
												   weights,
												   currCommunityAssignment, currVertex);

				eii[i] += communityDegreeMap[currCommunityAssignment[currVertex]];

				targetCommunityAssignment[i] = findTargetCommunityOfCurrVertex(communityDegreeMap,
																			selfLoop,
																			sizeOfCommunities,
																			degreesOfCommunities,
																			vertexDegrees[i],
																			currVertex,
																			constantForSecondTerm,
																			currCommunityAssignment);
			}
			else{
				targetCommunityAssignment[i] = -1;
			}

	         //Update
			// Cannot use double values for __sync_fetch* functions
			// Hence, ignoring the decimal part of the weights
	        if(targetCommunityAssignment[i] != currCommunityAssignment[currVertex]  && targetCommunityAssignment[i] != -1) {
	        	__sync_fetch_and_add(&updateDegreesOfCommunities[targetCommunityAssignment[i]], (unsigned long)vertexDegrees[i]);
	        	__sync_fetch_and_add(&updateSizeOfCommunities[targetCommunityAssignment[i]], 1);
	        	__sync_fetch_and_sub(&updateDegreesOfCommunities[currCommunityAssignment[currVertex]], (unsigned long)vertexDegrees[i]);
	        	__sync_fetch_and_sub(&updateSizeOfCommunities[currCommunityAssignment[currVertex]], 1);
	        }

	        communityDegreeMap.clear();
		}// End of for

//		// CALCULATE modularity
		currModularity = calculateModularity(eii, constantForSecondTerm,
											 degreesOfCommunities, numOfVertices, numOfVerticesOnProc);

		if(currModularity < prevModularity){
			break;
		}

		prevModularity = currModularity;
		if(prevModularity < Lower)
			prevModularity = Lower;

		MPI::COMM_WORLD.Allreduce(MPI::IN_PLACE, updateDegreesOfCommunities,
					  numOfVertices, MPI::LONG, MPI::SUM);
		MPI::COMM_WORLD.Allreduce(MPI::IN_PLACE, updateSizeOfCommunities,
							  numOfVertices, MPI::LONG, MPI::SUM);

		#pragma omp parallel for
		for(unsigned long i = 0; i < numOfVertices; i++){
			degreesOfCommunities[i] += updateDegreesOfCommunities[i];
			sizeOfCommunities[i] += updateSizeOfCommunities[i];
		}

		unsigned long* tmp;
		tmp = pastCommunityAssignment;
		pastCommunityAssignment = currCommunityAssignment; //Previous holds the current
		currCommunityAssignment = tmp; //Current holds the chosen assignment


		int numOfElementsToSend = numOfVerticesOnProc - offsetToSend;
		int* recvCounts = (int*)malloc(numProcs * sizeof(int));

		MPI::COMM_WORLD.Allgather(&numOfElementsToSend, 1, MPI::INT, recvCounts, 1, MPI::INT);

		//Calculate Prefix Sum
		int displacements[numProcs];
		for(int i = 0; i < numProcs; i++){
			if(i == 0) displacements[0] = 0;
			else{
				displacements[i] = displacements[i-1] + recvCounts[i-1];
			}
		}

		MPI::COMM_WORLD.Allgatherv(&targetCommunityAssignment[offsetToSend],
					   numOfElementsToSend,
					   MPI::UNSIGNED_LONG,
					   currCommunityAssignment,
					   recvCounts,
					   displacements,
					   MPI::UNSIGNED_LONG);

	}// end of while(true)
	time2 = omp_get_wtime();

	time = time2 - time1;

	MPI::COMM_WORLD.Allreduce(MPI::IN_PLACE, &time, 1, MPI::DOUBLE, MPI::MAX);
	*totTime = time;
	*numItr = numItrs;

	#pragma omp parallel for
	for (long i = 0; i < numOfVertices; i++) {
		communityInfo[i] = pastCommunityAssignment[i];
		assert(communityInfo[i] < numOfVertices);
	}

	free(degreesOfCommunities);
	free(pastCommunityAssignment);
	free(currCommunityAssignment);
	free(targetCommunityAssignment);
	free(vertexDegrees);
	free(sizeOfCommunities);
//	free(degreesOfCommunities);
	free(updateSizeOfCommunities);
	free(updateDegreesOfCommunities);

	return prevModularity;

}

void sumVertexDegrees(unsigned long *vertexStartPointers,
					  unsigned long *startVertices,
					  long *weights,
					  long *vertexDegrees,
					  unsigned long numOfVertices,
					  unsigned long numOfVerticesOnProc,
					  unsigned long numOfEdgesOnThisProc,
					  unsigned long *sizeOfCommunities,
					  long *degreesOfCommunities,
					  int *offsetToSend){

	#pragma omp parallel for
	for(unsigned long i = 0; i < numOfVerticesOnProc; i++){
		unsigned long start = vertexStartPointers[i];
		unsigned long end = i+1 == numOfVerticesOnProc ? numOfEdgesOnThisProc : vertexStartPointers[i+1];
		long totalWeight = 0;
		for(unsigned long j = start; j < end; j++){
			totalWeight += weights[j];
		}
		vertexDegrees[i] = totalWeight;
	}

	int size = MPI::COMM_WORLD.Get_size();
	int rank = MPI::COMM_WORLD.Get_rank();


	if(rank == 0){
		// send to and receive from 1
		sendRecvUpdateFromNextProc(startVertices, vertexDegrees, numOfVerticesOnProc, numOfEdgesOnThisProc, rank);
	}

	if(rank == size - 1){
		// send to and receive from rank - 1
		sendRecvUpdateFromPrevProc(startVertices, vertexDegrees, rank, offsetToSend);
	}

	if(rank > 0 && rank < size - 1){
		sendRecvUpdateFromNextProc(startVertices, vertexDegrees, numOfVerticesOnProc, numOfEdgesOnThisProc, rank);
		sendRecvUpdateFromPrevProc(startVertices, vertexDegrees, rank, offsetToSend);
	}

	int numOfElementsToSend = numOfVerticesOnProc - (*offsetToSend);
	int* recvCounts = (int*)malloc(size * sizeof(int));
	if(recvCounts != 0){
		int i = 0;
	}
//	if(rank == 0) cout<< "Before Allgather"<<endl;
	MPI::COMM_WORLD.Allgather(&numOfElementsToSend, 1, MPI::INT, recvCounts, 1, MPI::INT);

	//Calculate Prefix Sum
	int displacements[size];
	for(int i = 0; i < size; i++){
		if(i == 0) displacements[0] = 0;
		else{
			displacements[i] = displacements[i-1] + recvCounts[i-1];
		}
	}

//	if(rank == 0) cout<< "Before Allgatherv"<<endl;
	MPI::COMM_WORLD.Allgatherv(&vertexDegrees[*offsetToSend],
				   numOfElementsToSend,
				   MPI::LONG,
				   degreesOfCommunities,
				   recvCounts,
				   displacements,
				   MPI::LONG);//TODO

	#pragma omp parallel for
	for(unsigned long i = 0; i < numOfVertices; i++){
		sizeOfCommunities[i] = 1;
	}

}


//TODO: calculate the below sum using both MPI and openmp
double calculateConstantForSecondTerm(unsigned long numOfVertices,
									  long *degreesOfCommunities){

	int rank = MPI::COMM_WORLD.Get_rank();

	long totalWeight = 0;
	#pragma omp parallel for reduction(+:totalWeight)
	for(unsigned long i = 0; i < numOfVertices; i++)
		totalWeight += degreesOfCommunities[i];

//	MPI::COMM_WORLD.Allreduce(&partialTotalWeight,
//							  &globalTotalWeight,
//							  1,
//							  MPI::LONG,
//							  MPI::SUM);

	return 1/(double) totalWeight;
}

void initialCommunityAssignment(unsigned long *pastCommunityAssignment,
								unsigned long *currCommunityAssignment,
								unsigned long numOfVertices){
	#pragma omp parallel for
	for(unsigned long i = 0; i < numOfVertices; i++){
		pastCommunityAssignment[i] = i;
		currCommunityAssignment[i] = i;
	}
}

void sendRecvUpdateFromPrevProc(unsigned long *startVertices,
								long *vertexDegrees,
								int rank,
								int *offsetToSend){
	unsigned long neighborVertex = 0;

	long received = 0;

	MPI::COMM_WORLD.Sendrecv(&startVertices[0],
							 1,
							 MPI::UNSIGNED_LONG,
							 rank - 1,
							 0,
							 &neighborVertex,
							 1,
							 MPI::UNSIGNED_LONG,
							 rank - 1,
							 0);
	if(startVertices[0] == neighborVertex){
		(*offsetToSend)++;	//TODO
		MPI::COMM_WORLD.Sendrecv(&vertexDegrees[0],
								 1,
								 MPI::LONG,
								 rank - 1,
								 0,
								 &received,
								 1,
								 MPI::LONG,
								 rank - 1,
								 0);

		vertexDegrees[0] += received;
	}

}

void sendRecvUpdateFromNextProc(unsigned long *startVertices,
								long *vertexDegrees,
								unsigned long numOfVerticesOnProc,
								unsigned long numOfEdgesOnThisProc,
								int rank){
	unsigned long neighborVertex = 0;

	long received = 0;

	MPI::COMM_WORLD.Sendrecv(&startVertices[numOfEdgesOnThisProc - 1],
							 1,
							 MPI::UNSIGNED_LONG,
							 rank + 1,
							 0,
							 &neighborVertex,
							 1,
							 MPI::UNSIGNED_LONG,
							 rank + 1,
							 0);

	if(startVertices[numOfEdgesOnThisProc - 1] == neighborVertex){
		MPI::COMM_WORLD.Sendrecv(&vertexDegrees[numOfVerticesOnProc - 1],
								 1,
								 MPI::LONG,
								 rank + 1,
								 0,
								 &received,
								 1,
								 MPI::LONG,
								 rank + 1,
								 0);

		vertexDegrees[numOfVerticesOnProc - 1] += received;
	}

}

double buildCommunityDegreeMap(unsigned long startPointer, unsigned long endPointer,
		   	   	   	   	   	   map<unsigned long, long> &communityDegreeMap,
		   	   	   	   	   	   unsigned long *destinationVertices,
		   	   	   	   	   	   long *weights,
							   unsigned long *currCommunityAssignment, unsigned long currVertex){
	long selfLoop = 0;
	map<unsigned long, long>::iterator commDegreeMapIterator;
	for(unsigned long i = startPointer; i < endPointer; i++){
		if(destinationVertices[i] == currVertex){
			selfLoop += weights[i];
		}

		commDegreeMapIterator = communityDegreeMap.find(currCommunityAssignment[destinationVertices[i]]);
		if(commDegreeMapIterator != communityDegreeMap.end()){
			commDegreeMapIterator->second += weights[i];
		}
		else{
			communityDegreeMap[currCommunityAssignment[destinationVertices[i]]] = weights[i];
		}
	}

	return selfLoop;
}

unsigned long findTargetCommunityOfCurrVertex(map<unsigned long, long> &communityDegreeMap,
								long selfLoop,
								unsigned long *sizeOfCommunities,
								long *degreesOfCommunities,
								long vertexDegree,
								unsigned long currVertex,
								double constantForSecondTerm,
								unsigned long *currCommunityAssignment){

	map<unsigned long, long>::iterator commDegreeMapIterator;
	long maxIndex = currVertex;	//Assign the initial value as self community
	double curGain = 0;
	double maxGain = 0;
	unsigned long currVertexCommunity = currCommunityAssignment[currVertex];
	double eix = (double) (communityDegreeMap[currVertexCommunity]- selfLoop);
	double ax = (double) (degreesOfCommunities[currVertexCommunity] - vertexDegree);
	double eiy = 0;
	double ay = 0;

	commDegreeMapIterator = communityDegreeMap.begin();

	do{
		if(currVertexCommunity != commDegreeMapIterator->first) {
			ay = degreesOfCommunities[commDegreeMapIterator->first];
			eiy = commDegreeMapIterator->second;
		    curGain = 2*(eiy - eix) - 2*vertexDegree*(ay - ax)*constantForSecondTerm;

		    if( (curGain > maxGain) ||
		          ((curGain == maxGain) && (curGain != 0) && (commDegreeMapIterator->first < maxIndex)) ) {
		    	maxGain = curGain;
		    	maxIndex = commDegreeMapIterator->first;
		      }
		    }
		    commDegreeMapIterator++;
	}while(commDegreeMapIterator != communityDegreeMap.end());

	if(sizeOfCommunities[maxIndex] == 1 && sizeOfCommunities[currVertexCommunity] == 1 && maxIndex > currVertexCommunity){
	    maxIndex = currVertexCommunity;
	  }

	return maxIndex;

}

double calculateModularity(double *eii, double constantForSecondTerm,
						   long *degreesOfCommunities, unsigned long numOfVertices,
						   unsigned long numOfVerticesOnProc){

	int numProcs = MPI::COMM_WORLD.Get_size();
	int rank = MPI::COMM_WORLD.Get_rank();

	double partialE2_XX = 0;
	double partialA2_X = 0;

	double e_xx__a2_x[2] = {0, 0};

	#pragma omp parallel for reduction(+:partialE2_XX)
	for(unsigned long i = 0; i < numOfVerticesOnProc; i++){
		partialE2_XX += eii[i];
	}

	unsigned long* startIndices = (unsigned long*)malloc(numProcs * sizeof(unsigned long));
	unsigned long* endIndices = (unsigned long*)malloc(numProcs * sizeof(unsigned long));
	calculateStartAndEndIndices(startIndices, endIndices, numProcs, numOfVertices);

	#pragma omp parallel for reduction(+:partialA2_X)
	for(unsigned long i = startIndices[rank]; i < endIndices[rank]; i++){
		partialA2_X += (double) degreesOfCommunities[i] * (double) degreesOfCommunities[i];
	}

	double partial_e2_a2[2] = {partialE2_XX, partialA2_X};

	MPI::COMM_WORLD.Allreduce(&partial_e2_a2, &e_xx__a2_x, 2, MPI::DOUBLE, MPI::SUM);

	free(startIndices);
	free(endIndices);

	return (e_xx__a2_x[0]*constantForSecondTerm) - (e_xx__a2_x[1]*constantForSecondTerm*constantForSecondTerm);
}
