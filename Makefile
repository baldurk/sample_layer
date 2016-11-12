libsample_layer.so: sample_layer.cpp
	c++ -shared -fPIC -std=c++11 sample_layer.cpp -o libsample_layer.so
