// ***********************************************************************
//
//            Vite: A C++ library for distributed-memory graph clustering 
//                  using MPI+OpenMP
// 
//               Daniel Chavarria (daniel.chavarria@pnnl.gov)
//               Antonino Tumeo (antonino.tumeo@pnnl.gov)
//               Mahantesh Halappanavar (hala@pnnl.gov)
//               Pacific Northwest National Laboratory	
//
//               Hao Lu (luhowardmark@wsu.edu)
//               Sayan Ghosh (sayan.ghosh@wsu.edu)
//               Ananth Kalyanaraman (ananth@eecs.wsu.edu)
//               Washington State University
//
// ***********************************************************************
//
//       Copyright (2017) Battelle Memorial Institute
//                      All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
//
// 1. Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright
// notice, this list of conditions and the following disclaimer in the
// documentation and/or other materials provided with the distribution.
//
// 3. Neither the name of the copyright holder nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
// FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
// COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
// INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
// BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
// LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
// LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
// ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//
// ************************************************************************

#include <fstream>
#include <iostream>
#include <numeric>
#include <sstream>
#include <vector>
#include <utility>
#include <map>
#include <unordered_map>
#include <climits>

#include "parallel-shards.hpp"

/// Pairwise read files between fileStartIndex and fileEndIndex and store 
/// it into a single file, also selectively convert to binary

/// fileInShardsPath expects the input directory where the shards are ('/' is not allowed at the end)

/// the input file is expected to contain 4 data points in every line:
/// ----ai, aj, common, jaccard---- (common is not processed at present)

/// We expect the shards to contain the 'upper triangle' of the adjacency...during binary 
/// conversion, we consider the data to be an undirected graph, and store both combinations of edge pairs

void loadParallelFileShards(int rank, int nprocs, int naggr, 
	const std::string &fileInShardsPath, const std::string &fileOutPath, 
        const int fileStartIndex, const int fileEndIndex, bool indexOneBased, 
        Weight_t wtype, GraphElem shardCount)
{
  assert(fileStartIndex >= 0);
  assert(fileEndIndex >= 0);
  assert(fileEndIndex >= fileStartIndex);

  GraphElem numEdges = 0, numVertices = -1;
  int file_open_error;
  MPI_File fh;

  /// Part 1: Read the file shards into edge list  
  
  std::vector<GraphElemTuple> edgeList;
  std::map<GraphElem, std::string> fileProc;

  // make a list of the files and processes
  GraphElem proc = 0;
  for (GraphElem ci = fileStartIndex; ci < fileEndIndex + 1; ci++) {
      for (GraphElem cj = fileStartIndex; cj < fileEndIndex + 1; cj++) {

          // construct file name
          std::string fileNameShard = std::to_string(ci) + "__" + std::to_string(cj) + ".csv";
          std::string fileName = fileInShardsPath + "/" + fileNameShard;

          // check if file exists
          std::ifstream ifs;
          ifs.open(fileName.c_str(), std::ifstream::in);

          if (ifs.fail())
              continue;

          // push in the dictionary
          fileProc.insert(std::pair<GraphElem, std::string>(proc, fileName));

          proc++;

          ifs.close();
      }
  }

  MPI_Barrier(MPI_COMM_WORLD);

  GraphElem v_hi = -1, v_lo = -1;

  // read the files only if I can
  std::map<GraphElem, std::string>::iterator mpit = fileProc.find(rank);
  
  if (mpit != fileProc.end()) {
	  // retrieve lo/hi range from file name string
	  std::string fileName_full = (mpit->second).substr((mpit->second).find_last_of("/") + 1);
	  std::string fileName_noext = fileName_full.substr(0, fileName_full.find("."));
	  std::string fileName_right = fileName_noext.substr(fileName_full.find("__") + 2);
	  std::string fileName_left = fileName_noext.substr(0, fileName_full.find("__"));

	  v_lo = (GraphElem)(std::stoi(fileName_left) - 1)*shardCount;
	  v_hi = (GraphElem)(std::stoi(fileName_right) - 1)*shardCount;
#if defined(DEBUG_PRINTF)
	  std::cout << "File processing: " << fileName_full << "; Ranges: " << v_lo  << ", " << v_hi << std::endl;
#endif
	  // open file shard and start reading
	  std::ifstream ifs;
	  ifs.open((mpit->second).c_str(), std::ifstream::in);

	  std::string line;

	  while(!ifs.eof()) {

		  GraphElem v0, v1, info;
		  GraphWeight w;
		  char ch;

		  std::getline(ifs, line);
		  std::istringstream iss(line);

		  // read from current shard 
                  if (wtype == ORG_WEIGHT)
                      iss >> v0 >> ch >> v1 >> ch >> info >> ch >> w;
                  if (wtype == ABS_WEIGHT) {
                      iss >> v0 >> ch >> v1 >> ch >> info >> ch >> w;
                      w = std::fabs(w);
                  }
                  else
                      iss >> v0 >> ch >> v1 >> ch >> info;

		  if (indexOneBased) {
			  v0--; 
			  v1--;
		  }

		  // normalize v0/v1 by adding lo/hi shard ID
		  v0 += v_lo;
		  v1 += v_hi;

		  edgeList.push_back({v0, v1, w});
		  edgeList.push_back({v1, v0, w});

		  if (v0 > numVertices)
                      numVertices = v0;
		  if (v1 > numVertices)
                      numVertices = v1;
	  }

	  // close current shard
	  ifs.close();
  }

  numEdges = edgeList.size();

  // idle processes wait at the barrier
  MPI_Barrier(MPI_COMM_WORLD);

  const int elprocs = fileProc.size();
  fileProc.clear();

  // numEdges/numVertices to be written by process 0
  GraphElem globalNumVertices = 0, globalNumEdges = 0;
  
  MPI_Allreduce(&numEdges, &globalNumEdges, 1, MPI_GRAPH_TYPE, MPI_SUM, MPI_COMM_WORLD);
  MPI_Allreduce(&numVertices, &globalNumVertices, 1, MPI_GRAPH_TYPE, MPI_MAX, MPI_COMM_WORLD);
  
  if (!indexOneBased)
	  globalNumVertices += 1;

  if (rank == 0)
      std::cout << "Graph #nvertices: " << globalNumVertices << ", #edges: " << globalNumEdges << std::endl;

  /// Part 2: Count number of edges and sort edges locally
  // assumed vertex-based distribution to build edgeCount
  std::vector<GraphElem> parts(nprocs+1); 
  parts[0] = 0;

  for (GraphElem i = 1; i < nprocs+1; i++)
      parts[i] = (((globalNumVertices+1) * i) / nprocs); 
  
  GraphElem alocalNumVertices = (((globalNumVertices+1)*(rank + 1)) / nprocs) - (((globalNumVertices+1)*rank) / nprocs); 

  std::vector<GraphElem> edgeCount(alocalNumVertices);
  std::vector<std::unordered_map<GraphElem, int>> outVertices(nprocs);
  
  // check vertex end points and perform local counting
  // fill outgoing buffer for ghosts
  std::vector<GraphElem>::iterator iter; 
  int owner;
  for (GraphElem i = 0; i < numEdges; i++) {
      GraphElem vertex = edgeList[i].i_+1;

      // get owner
      iter = std::upper_bound(parts.begin(), parts.end(), vertex);
      owner = (iter - parts.begin() - 1);

      if (owner == rank)
          edgeCount[vertex - parts[rank]]++; // global to local index translation
      else {
          if (outVertices[owner].find(vertex) != outVertices[owner].end())
              outVertices[owner][vertex] += 1;
          else
              outVertices[owner].insert(std::pair<GraphElem, int>(vertex, 1));
      }

      // repeat for another end point
      vertex = edgeList[i].j_+1;

      // get owner
      iter = std::upper_bound(parts.begin(), parts.end(), vertex);
      owner = (iter - parts.begin() - 1);

      if (owner == rank)
          edgeCount[vertex - parts[rank]]++; // global to local index translation
      else {
          if (outVertices[owner].find(vertex) != outVertices[owner].end())
              outVertices[owner][vertex] += 1;
          else
              outVertices[owner].insert(std::pair<GraphElem, int>(vertex, 1));
      }
  }

  if (rank == 0)
      std::cout << "Local counting of edges done, prepared outgoing vertex list with local counts." << std::endl;

  MPI_Barrier(MPI_COMM_WORLD);

  // exchange count information
  std::vector<int> ssize(nprocs), rsize(nprocs), sdispls(nprocs), rdispls(nprocs);

  // outVertices contains outgoing {edges,frequency} information
  // copy from map to linear buffer, *2 because we store both key and val
  int spos = 0;
  for (int p = 0; p < nprocs; p++) {
      ssize[p] = (int)(outVertices[p].size() * 2);
      sdispls[p] = spos;
      spos += ssize[p];
  }

  std::vector<GraphElem> sredata(spos);
  spos = 0;
  for (int p = 0; p < nprocs; p++) {
      int idx = 0;
      for (std::unordered_map<GraphElem, int>::iterator it = outVertices[p].begin(); it != outVertices[p].end(); ++it) {
          sredata[spos+idx] = it->first;
          sredata[spos+idx+1] = it->second;
          idx += 2;
      }
      spos += (outVertices[p].size() * 2);
  }

  for (int p = 0; p < nprocs; p++)
      outVertices[p].clear();
  outVertices.clear();
  
  MPI_Alltoall(ssize.data(), 1, MPI_INT, rsize.data(), 1, MPI_INT, MPI_COMM_WORLD);
  
  int rpos = 0;
  for (int p = 0; p < nprocs; p++) {
      rdispls[p] = rpos;
      rpos += rsize[p];
  }

  std::vector<GraphElem> rredata(rpos);
  MPI_Alltoallv(sredata.data(), ssize.data(), sdispls.data(), 
          MPI_GRAPH_TYPE, rredata.data(), rsize.data(), rdispls.data(), 
          MPI_GRAPH_TYPE, MPI_COMM_WORLD);
   
  if (rank == 0)
      std::cout << "Exchanged vertex and count information." << std::endl;

 
  // perform counting for remote edge tails 
  // obtained from alltoallv
  for (GraphElem i = 0; i < rpos; i+=2) {
      edgeCount[rredata[i] - parts[rank]] += rredata[i+1]; 
  }
  
  if (rank == 0)
      std::cout << "Updated vertex counts, to begin prefix sum for building CSR." << std::endl;

  sredata.clear();
  rredata.clear();
  ssize.clear();
  rsize.clear();
  sdispls.clear();
  rdispls.clear();
  parts.clear();

  // local prefix sum
  std::vector<GraphElem> ecTmp(alocalNumVertices);
  std::partial_sum(edgeCount.begin(), edgeCount.end(), ecTmp.begin());
  edgeCount = ecTmp;
  ecTmp.clear();
  
  MPI_Barrier(MPI_COMM_WORLD);

  // distributed prefix sum of coefficient
  GraphElem psum = edgeCount.back();
  MPI_Exscan(MPI_IN_PLACE, &psum, 1, MPI_GRAPH_TYPE, MPI_SUM, MPI_COMM_WORLD);

  if (rank != 0) {
      for (GraphElem i = 0; i < alocalNumVertices; i++)
          edgeCount[i] += psum;
  }
  
  if (rank == 0)
      std::cout << "Completed distributed prefix sum for building CSR." << std::endl;

  MPI_Barrier(MPI_COMM_WORLD);
  
  // local sorting of edge list
  auto ecmp = [] (GraphElemTuple const& e0, GraphElemTuple const& e1)
  { return ((e0.i_ < e1.i_) || ((e0.i_ == e1.i_) && (e0.j_ < e1.j_))); };

  if (!std::is_sorted(edgeList.begin(), edgeList.end(), ecmp)) {
#if defined(DEBUG_PRINTF)
	  std::cout << "Edge list is not sorted" << std::endl;
#endif
	  std::sort(edgeList.begin(), edgeList.end(), ecmp);
  }
  else {
#if defined(DEBUG_PRINTF)
	  std::cout << "Edge list is sorted!" << std::endl;
#endif
  }

  MPI_Barrier(MPI_COMM_WORLD);

  /// Part 3: Dump the data to a binary file   
  file_open_error = MPI_File_open(MPI_COMM_WORLD, fileOutPath.c_str(), 
          MPI_MODE_CREATE | MPI_MODE_WRONLY, MPI_INFO_NULL, &fh); 

  if (file_open_error != MPI_SUCCESS) {
      std::cout<< " Error opening output file! " << std::endl;
      MPI_Abort(-99, MPI_COMM_WORLD);
  }
  
  // process 0 writes the #vertices/edges first, followed by edgeCount
  if (rank == 0) {
      std::cout << "Processing complete, about to write the binary file." << std::endl;
      MPI_File_write_at(fh, 0, &globalNumVertices, sizeof(GraphElem), MPI_BYTE, MPI_STATUS_IGNORE);
      MPI_File_write_at(fh, sizeof(GraphElem), &globalNumEdges, sizeof(GraphElem), MPI_BYTE, MPI_STATUS_IGNORE);
  }
  
  MPI_Barrier(MPI_COMM_WORLD);

  // write the edge prefix counts first (required for CSR 
  // construction during reading)
  GraphElem ec_offset = 0;
  MPI_Exscan(&alocalNumVertices, &ec_offset, 1, MPI_GRAPH_TYPE, MPI_SUM, MPI_COMM_WORLD);

  uint64_t tot_bytes = alocalNumVertices * sizeof(GraphElem);
  MPI_Offset offset = 2*sizeof(GraphElem) + ec_offset*sizeof(GraphElem);

  if (tot_bytes < INT_MAX)
      MPI_File_write_at(fh, offset, edgeCount.data(), tot_bytes, MPI_BYTE, MPI_STATUS_IGNORE);
  else {
      int chunk_bytes=INT_MAX;
      uint8_t *curr_pointer = (uint8_t*) edgeCount.data();
      uint64_t transf_bytes=0;

      while (transf_bytes<tot_bytes)
      {
          MPI_File_write_at(fh, offset, curr_pointer, chunk_bytes, MPI_BYTE, MPI_STATUS_IGNORE);
          transf_bytes+=chunk_bytes;
          offset+=chunk_bytes;
          curr_pointer+=chunk_bytes;

          if (tot_bytes-transf_bytes<INT_MAX)
              chunk_bytes=tot_bytes-transf_bytes;
      } 
  }

  if (rank == 0)
      std::cout << "Beginning to write the second part of the binary file (edges) using " << elprocs << " processes." << std::endl;      

  // write the edge list next
  tot_bytes = numEdges * sizeof(Edge);

  GraphElem e_offset = 0;
  MPI_Exscan(&numEdges, &e_offset, 1, MPI_GRAPH_TYPE, MPI_SUM, MPI_COMM_WORLD);

  MPI_Barrier(MPI_COMM_WORLD);
  
  // participate in writing only if a process has something to contribute
  if (tot_bytes > 0) {
	  
	  offset = 2*sizeof(GraphElem) + (globalNumVertices+1)*sizeof(GraphElem) + e_offset*(sizeof(Edge));
	  
	  if (tot_bytes<INT_MAX)
		  MPI_File_write_at(fh, offset, edgeList.data(), tot_bytes, MPI_BYTE, MPI_STATUS_IGNORE);
	  else {
		  int chunk_bytes=INT_MAX;
		  uint8_t *curr_pointer = (uint8_t*)edgeList.data();
		  uint64_t transf_bytes=0;

		  while (transf_bytes<tot_bytes)
		  {
			  MPI_File_write_at(fh, offset, curr_pointer, chunk_bytes, MPI_BYTE, MPI_STATUS_IGNORE);
			  transf_bytes+=chunk_bytes;
			  offset+=chunk_bytes;
			  curr_pointer+=chunk_bytes;

			  if (tot_bytes-transf_bytes<INT_MAX)
				  chunk_bytes=tot_bytes-transf_bytes;
		  } 
	  }
  }

  MPI_Barrier(MPI_COMM_WORLD);

  MPI_File_close(&fh);

  if (rank == 0)
      std::cout << "Completed writing the binary file: " << fileOutPath << std::endl;      

  edgeCount.clear();
  edgeList.clear();
} // loadParallelFileShards
