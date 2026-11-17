# The stub provider serves the F0, Note and Align contracts without a model, so that the package
# path, interpreter discovery and the executive lifecycle are tested without one. It registers
# itself as a static plugin of the inference category. An object library hands every object to the
# test that links it, whereas a static library hands over only the objects that the test
# references, and nothing references the object that holds the registration.
add_library(otterstub OBJECT
    StubInterpreters.h
    StubInterpreters.cpp
    main.cpp
)
target_link_libraries(otterstub PUBLIC synthrt-otter analysisutil)
target_compile_features(otterstub PUBLIC cxx_std_17)
