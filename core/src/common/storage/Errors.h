#ifndef FROND_STORAGE_ERRORS_H
#define FROND_STORAGE_ERRORS_H

#include <exception>
#include <string>

// What CardFile and FatImage throw. `message` is shown to the user as it is.
namespace frondStorage {

class Error : public std::exception {
   public:
    explicit Error(std::string text) : message(std::move(text)) {}
    const char* what() const noexcept override { return message.c_str(); }
    std::string message;
};

// A change stopped by its caller; what was done stays, the rest is rolled back.
class Cancelled : public Error {
   public:
    Cancelled() : Error("Cancelled.") {}
};

}  // namespace frondStorage

#endif
