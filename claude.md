# Application to connect to a reolink cctv camera and receive a video stream and display it in a window

# Architecture

A c++ application that connects to a reolink cctv camera, using the native bachuan protocol and receives the video stream to display it in a window

# Background info
* this is based on a blog written about connecting to a reolink camera - see https://www.thirtythreeforty.net/posts/2020/05/hacking-reolink-cameras-for-fun-and-profit/
* Rust source code is in the @neolink directory (but we are only interested in the specific code for connecting and authorising using bachuan and obtaining the video code)
* the destination application code will be written in c++ and written into the @bachuan folder

# Development libraries
* analyse the @neolink folder for the specific files that implement the bachuan protocol
* convert these to c++ in the @bachuan folder
* use cmake as the compilation mechanism for linux
* you can use the OpenCV library and ffmpeg libraries for interacting with the video stream 
* display the video using a gtk window format

# Testing

A reolink camera can be found at 10.0.1.29 with username "admin"