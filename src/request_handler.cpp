//
// request_handler.cpp
// ~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2017 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#include <mjpeg-server/request_handler.hpp>
#include <mjpeg-server/mime_types.hpp>
#include <mjpeg-server/reply.hpp>
#include <mjpeg-server/request.hpp>

#include <raspicam/raspicam.h>

#include <turbojpeg.h>

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>


namespace http{ namespace server{


	std::string to_jpg_image(
		std::uint8_t const* img,
		std::size_t const width,
		std::size_t const height,
		int const quality
	){
		// JPEG encoding
		auto compressor_deleter = [](void* data){ tjDestroy(data); };
		std::unique_ptr< void, decltype(compressor_deleter) > compressor(
			tjInitCompress(),
			compressor_deleter
		);
		if(!compressor) throw std::runtime_error("tjInitCompress failed");

		// Image buffer
		std::uint8_t* data = nullptr;
		unsigned long size = 0;

		if(tjCompress2(
			compressor.get(),
			const_cast< std::uint8_t* >(img),
			static_cast< int >(width),
			static_cast< int >(width * 3),
			static_cast< int >(height),
			TJPF_RGB,
			&data,
			&size,
			TJSAMP_420,
			quality,
			0
		) != 0){
			throw std::runtime_error(std::string("tjCompress2 failed: ")
				+ tjGetErrorStr());
		}

		auto data_deleter = [](std::uint8_t* data){ tjFree(data); };
		std::unique_ptr< std::uint8_t, decltype(data_deleter) > data_ptr(
			data,
			data_deleter
		);

		// output
		return std::string(data_ptr.get(), data_ptr.get() + size);
	}



	request_handler::request_handler(const std::string& doc_root)
		: doc_root_(doc_root)
		{}

	void request_handler::handle_request(const request& req, reply& rep){
		// Decode url to path.
		std::string request_path;
		if(!url_decode(req.uri, request_path)){
			rep = reply::stock_reply(reply::bad_request);
			return;
		}

		// Request path must be absolute and not contain "..".
		if(request_path.empty() || request_path[0] != '/'
				|| request_path.find("..") != std::string::npos){
			rep = reply::stock_reply(reply::bad_request);
			return;
		}

		// If path ends in slash (i.e. is a directory) then add "index.html".
		if(request_path[request_path.size() - 1] == '/'){
			request_path += "index.html";
		}

		// Determine the file extension.
		std::size_t last_slash_pos = request_path.find_last_of("/");
		std::size_t last_dot_pos = request_path.find_last_of(".");
		std::string extension;
		if(last_dot_pos != std::string::npos && last_dot_pos > last_slash_pos){
			extension = request_path.substr(last_dot_pos + 1);
		}

		if(request_path == "/live"){
			raspicam::RaspiCam cam_;
			cam_.setFormat(raspicam::RASPICAM_FORMAT_BGR);
			if(!cam_.open(true)){
				throw std::runtime_error("Can not connect to raspicam");
			}

			cam_.grab();
			auto const width = cam_.getWidth();
			auto const height = cam_.getHeight();
			std::cout << "grab image: " << width << "x" << height << std::endl;
			std::uint8_t const* const data = cam_.getImageBufferData();
			if(data == nullptr){
				throw std::runtime_error("raspicam getImageBufferData failed");
			}
			rep.content = to_jpg_image(data, width, height, 85);
			extension = "jpg";
		}else{
			// Open the file to send back.
			std::string full_path = doc_root_ + request_path;
			std::ifstream is(full_path.c_str(), std::ios::in | std::ios::binary);
			if(!is){
				rep = reply::stock_reply(reply::not_found);
				return;
			}

			// Fill out the reply to be sent to the client.
			rep.status = reply::ok;
			char buf[512];
			while(is.read(buf, sizeof(buf)).gcount() > 0){
				rep.content.append(buf, is.gcount());
			}
		}

		rep.headers.resize(2);
		rep.headers[0].name = "Content-Length";
		rep.headers[0].value = std::to_string(rep.content.size());
		rep.headers[1].name = "Content-Type";
		rep.headers[1].value = mime_types::extension_to_type(extension);
	}

	bool request_handler::url_decode(const std::string& in, std::string& out){
		out.clear();
		out.reserve(in.size());
		for(std::size_t i = 0; i < in.size(); ++i){
			if(in[i] == '%'){
				if(i + 3 <= in.size()){
					int value = 0;
					std::istringstream is(in.substr(i + 1, 2));
					if(is >> std::hex >> value){
						out += static_cast<char>(value);
						i += 2;
					}else{
						return false;
					}
				}else{
					return false;
				}
			}else if(in[i] == '+'){
				out += ' ';
			}else{
				out += in[i];
			}
		}
		return true;
	}


} }
