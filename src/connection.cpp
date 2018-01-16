//
// connection.cpp
// ~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2017 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#include <mjpeg-server/connection.hpp>
#include <mjpeg-server/connection_manager.hpp>
#include <mjpeg-server/request_handler.hpp>

#include <raspicam/raspicam.h>

#include <turbojpeg.h>

#include <iostream>
#include <utility>
#include <vector>


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


	connection::connection(
		boost::asio::ip::tcp::socket socket,
		connection_manager& manager,
		request_handler& handler
	)
		: socket_(std::move(socket))
		, connection_manager_(manager)
		, request_handler_(handler)
		{}

	void connection::start(){
		do_read();
	}

	void connection::stop(){
		socket_.close();
	}

	void connection::do_read(){
		auto self(shared_from_this());
		socket_.async_read_some(boost::asio::buffer(buffer_),
			[this, self](boost::system::error_code ec, std::size_t bytes_transferred){
				if(!ec){
					request_parser::result_type result;
					std::tie(result, std::ignore) = request_parser_.parse(
							request_, buffer_.data(), buffer_.data() + bytes_transferred);

					if(result == request_parser::good){
						bool is_mjpeg
							= request_handler_.handle_request(request_, reply_);

						if(!is_mjpeg){
							do_write();
							return;
						}

						try{
							boost::asio::write(socket_, reply_.to_buffers());

							raspicam::RaspiCam cam_;
							cam_.setFormat(raspicam::RASPICAM_FORMAT_BGR);
							if(!cam_.open(true)){
								throw std::runtime_error("Can not connect to raspicam");
							}

							for(;;){
								cam_.grab();
								auto const width = cam_.getWidth();
								auto const height = cam_.getHeight();
								std::cout << "grab image: " << width << "x" << height << std::endl;
								std::uint8_t const* const data = cam_.getImageBufferData();
								if(data == nullptr){
									throw std::runtime_error("raspicam getImageBufferData failed");
								}
								auto img = to_jpg_image(data, width, height, 75);
								auto content =
									"--mjpeg\r\n"
									"Content-Type:image/jpeg\r\n"
									"Content-Length:" + std::to_string(img.size()) + img + "\r\n"
									"\r\n" + img + "\r\n";
								boost::asio::write(socket_, boost::asio::buffer(content));
								std::this_thread::sleep_for(std::chrono::milliseconds(2000));
							}
						}catch(std::exception& e){
							std::cout << "exception: " << e.what() << "\n";
							connection_manager_.stop(shared_from_this());
						}
					}else if(result == request_parser::bad){
						reply_ = reply::stock_reply(reply::bad_request);
						do_write();
					}else{
						do_read();
					}
				}else if(ec != boost::asio::error::operation_aborted){
					connection_manager_.stop(shared_from_this());
				}
			});
	}

	void connection::do_write(){
		auto self(shared_from_this());
		boost::asio::async_write(socket_, reply_.to_buffers(),
			[this, self](boost::system::error_code ec, std::size_t){
				if(!ec){
					// Initiate graceful connection closure.
					boost::system::error_code ignored_ec;
					socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_both,
						ignored_ec);
				}

				if(ec != boost::asio::error::operation_aborted){
					connection_manager_.stop(shared_from_this());
				}
			});
	}


} }
