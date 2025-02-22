﻿// Copyright (c) 2022 Computer Vision Center (CVC) at the Universitat Autonoma
// de Barcelona (UAB).
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

#include "carla/multigpu/incomingMessage.h" // 引入CARLA多GPU通信模块中负责处理接收消息的头文件
// 此头文件可能定义了incomingMessage类或其他相关结构，用于处理从其他GPU或节点接收的数据包。

#include "carla/multigpu/secondary.h"       // 引入CARLA多GPU框架中提供次要或辅助功能的头文件
// 此头文件可能包含了执行次要计算任务、数据预处理或后处理等功能的类和方法。

#include "carla/BufferPool.h"                // 引入CARLA框架中管理内存缓冲区的头文件
// BufferPool类可能提供了缓冲区的分配、释放和重用功能，以优化内存使用和性能。

#include "carla/Debug.h"                     // 引入CARLA框架中用于调试功能的头文件
// 此头文件可能定义了调试宏、函数或类，用于在开发过程中输出调试信息、检查断言等。

#include "carla/Exception.h"                 // 引入CARLA框架中处理异常的头文件
// Exception类及其相关可能定义了自定义异常类型，用于在框架中处理错误情况。

#include "carla/Logging.h"                   // 引入CARLA框架中负责日志记录的头文件
// Logging类和相关可能提供了灵活的日志记录功能，包括不同级别的日志信息（如调试、信息、警告、错误等）。

#include "carla/Time.h"                      // 引入CARLA框架中处理时间和计时功能的头文件
// Time类及相关可能提供了时间戳获取、时间间隔测量、定时器等功能。

#include <boost/asio/connect.hpp>            // 引入Boost.Asio库中负责建立连接的头文件
// connect函数模板用于启动到远程端点的异步或同步连接。

#include <boost/asio/read.hpp>               // 引入Boost.Asio库中负责读取数据的头文件
// read和async_read函数模板用于从异步操作关联的流对象中读取数据。

#include <boost/asio/write.hpp>              // 引入Boost.Asio库中负责写入数据的头文件
// write和async_write函数模板用于向异步操作关联的流对象中写入数据。

#include <boost/asio/post.hpp>               // 引入Boost.Asio库中用于异步任务调度的头文件
// post函数模板用于将任务（函数对象、lambda表达式等）排队到指定的I/O执行器上，以便稍后异步执行。

#include <boost/asio/bind_executor.hpp>      // 引入Boost.Asio库中用于绑定执行器的头文件
// bind_executor函数模板用于创建一个与指定执行器关联的可调用对象，确保任务在正确的执行器上执行。

#include <exception>                         // 引入C++标准库中的异常处理头文件
// 此头文件定义了std::exception类及其相关功能，用于在程序中处理异常和错误情况。
namespace carla {
namespace multigpu {

  Secondary::Secondary(                    // 构造函数，接受端点和回调函数
    boost::asio::ip::tcp::endpoint ep,
    SecondaryCommands::callback_type callback) :
      _pool(),                              // 初始化缓冲池
      _socket(_pool.io_context()),          // 初始化套接字
      _endpoint(ep),                        // 设置端点
      _strand(_pool.io_context()),          // 初始化strand以确保线程安全
      _connection_timer(_pool.io_context()),// 初始化连接计时器
      _buffer_pool(std::make_shared<BufferPool>()) { // 创建共享的缓冲池

      _commander.set_callback(callback);    // 设置回调函数
    }


  Secondary::Secondary(                    // 另一个构造函数，接受IP和端口以及回调函数
    std::string ip,
    uint16_t port,
    SecondaryCommands::callback_type callback) :
      _pool(),                              // 初始化缓冲池
      _socket(_pool.io_context()),          // 初始化套接字
      _strand(_pool.io_context()),          // 初始化strand以确保线程安全
      _connection_timer(_pool.io_context()),// 初始化连接计时器
      _buffer_pool(std::make_shared<BufferPool>()) { // 创建共享的缓冲池

    boost::asio::ip::address ip_address = boost::asio::ip::address::from_string(ip); // 从字符串转换为IP地址
    _endpoint = boost::asio::ip::tcp::endpoint(ip_address, port); // 设置端点
    _commander.set_callback(callback);    // 设置回调函数
  }

  Secondary::~Secondary() {                // 析构函数
    _pool.Stop();                          // 停止缓冲池
  }

  void Secondary::Connect() {              // 连接函数
    AsyncRun(2u);                          // 启动异步运行，使用2个工作线程

    _commander.set_secondary(shared_from_this()); // 设置当前对象为命令的次要部分

    std::weak_ptr<Secondary> weak = shared_from_this(); // 创建弱指针以防止循环引用
    boost::asio::post(_strand, [weak]() { // 在strand中发布任务
      auto self = weak.lock();             // 锁定弱指针
      if (!self) return;                   // 如果对象已被销毁，返回

      if (self->_done) {                   // 如果已完成
        return;
      }

      if (self->_socket.is_open()) {       // 如果套接字是打开的
        self->_socket.close();              // 关闭套接字
      }

      auto handle_connect = [weak](boost::system::error_code ec) { // 处理连接结果的回调
        auto self = weak.lock();            // 锁定弱指针
        if (!self) return;                  // 如果对象已被销毁，返回
        if (ec) {                           // 如果有错误
          log_error("secondary server: connection failed:", ec.message()); // 记录错误信息
          if (!self->_done)                 // 如果未完成，尝试重连
            self->Reconnect();
          return;
        }

        if (self->_done) {                  // 如果已完成
          return;
        }

        // 此设置强制不使用Nagle算法。
        // 在Linux上提高同步模式速度约3倍。
        self->_socket.set_option(boost::asio::ip::tcp::no_delay(true)); // 禁用Nagle算法

        log_info("secondary server: connected to ", self->_endpoint); // 记录连接信息

        self->ReadData();                   // 调用读取数据函数
      };

      self->_socket.async_connect(self->_endpoint, boost::asio::bind_executor(self->_strand, handle_connect)); // 异步连接
    });
  }

  void Secondary::Stop() {                 // 停止函数
    _connection_timer.cancel();             // 取消连接计时器
    std::weak_ptr<Secondary> weak = shared_from_this(); // 创建弱指针
    boost::asio::post(_strand, [weak]() {   // 在strand中发布停止任务
      auto self = weak.lock();              // 锁定弱指针
      if (!self) return;                    // 如果对象已被销毁，返回
      self->_done = true;                   // 标记为已完成
      if (self->_socket.is_open()) {       // 如果套接字是打开的
        self->_socket.close();              // 关闭套接字
      }
    });
  }

  void Secondary::Reconnect() {             // 重连函数
    std::weak_ptr<Secondary> weak = shared_from_this(); // 创建弱指针
    _connection_timer.expires_from_now(time_duration::seconds(1u)); // 设置计时器为1秒后到期
    _connection_timer.async_wait([weak](boost::system::error_code ec) { // 等待计时器到期后的回调
      auto self = weak.lock();              // 锁定弱指针
      if (!self) return;                    // 如果对象已被销毁，返回
      if (!ec) {                            // 如果没有错误
        self->Connect();                    // 重新连接
      }
    });
  }

  void Secondary::AsyncRun(size_t worker_threads) { // 启动异步运行，接受工作线程数量
    _pool.AsyncRun(worker_threads);         // 调用缓冲池的异步运行
  }

  void Secondary::Write(std::shared_ptr<const carla::streaming::detail::tcp::Message> message) { // 写入函数
    DEBUG_ASSERT(message != nullptr);       // 确保消息不为空
    DEBUG_ASSERT(!message->empty());        // 确保消息不为空
    std::weak_ptr<Secondary> weak = shared_from_this(); // 创建弱指针
    boost::asio::post(_strand, [=]() {      // 在strand中发布写入任务
      auto self = weak.lock();              // 锁定弱指针
      if (!self) return;                    // 如果对象已被销毁，返回
      if (!self->_socket.is_open()) {      // 如果套接字未打开
        return;                             // 返回
      }

      auto handle_sent = [weak, message](const boost::system::error_code &ec, size_t DEBUG_ONLY(bytes)) { // 处理发送结果的回调
        auto self = weak.lock();             // 锁定弱指针
        if (!self) return;                   // 如果对象已被销毁，返回
        if (ec) {                           // 如果有错误
          log_error("error sending data: ", ec.message()); // 记录发送数据的错误
        }
      };

     // 设置超时期限
      boost::asio::async_write(
          self->_socket,// 目标 socket
          message->GetBufferSequence(),// 获取要发送的消息缓冲区序列
          boost::asio::bind_executor(self->_strand, handle_sent)); // 绑定执行器，处理发送完成后的回调
    });
  }

  void Secondary::Write(Buffer buffer) {
// 创建一个视图从传入的缓冲区
    auto view_data = carla::BufferView::CreateFrom(std::move(buffer));
 // 创建消息
    auto message = Secondary::MakeMessage(view_data);

    DEBUG_ASSERT(message != nullptr);// 确保消息不为空
    DEBUG_ASSERT(!message->empty());// 确保消息不为空
    std::weak_ptr<Secondary> weak = shared_from_this(); // 创建弱指针以避免循环引用
    boost::asio::post(_strand, [=]() {// 在strand中异步执行
      auto self = weak.lock(); // 锁定弱指针
      if (!self) return;// 如果对象已被销毁，返回
      if (!self->_socket.is_open()) {// 如果socket未打开
        return;
      }

     // 发送完成后的处理函数
      auto handle_sent = [weak, message](const boost::system::error_code &ec, size_t DEBUG_ONLY(bytes)) {
        auto self = weak.lock(); // 锁定弱指针
        if (!self) return;// 如果对象已被销毁，返回
        if (ec) {// 如果发生错误
          log_error("error sending data: ", ec.message()); // 记录错误
        }
      };

      // 设置超时（注释掉）
      boost::asio::async_write(
          self->_socket,// 异步写入socket
          message->GetBufferSequence(),// 获取消息的缓冲区序列
          boost::asio::bind_executor(self->_strand, handle_sent));// 绑定执行器
    });
  }

  void Secondary::Write(std::string text) {
    std::weak_ptr<Secondary> weak = shared_from_this(); // 创建弱指针以避免循环引用
    boost::asio::post(_strand, [=]() {
      auto self = weak.lock();// 锁定弱指针
      if (!self) return;// 如果对象已被销毁，返回
      if (!self->_socket.is_open()) {// 如果socket未打开
        return;
      }

     // 发送完成后的处理函数
      auto handle_sent = [weak](const boost::system::error_code &ec, size_t DEBUG_ONLY(bytes)) {
        auto self = weak.lock();// 锁定弱指针
        if (!self) return;// 如果对象已被销毁，返回
        if (ec) { // 如果发生错误
          log_error("error sending data: ", ec.message());// 记录错误
        }
      };

     // 设置超时（注释掉）
      // 发送大小缓冲区
      int this_size = text.size();// 获取字符串大小
      boost::asio::async_write(
          self->_socket,// 异步写入socket
          boost::asio::buffer(&this_size, sizeof(this_size)),// 写入大小
          boost::asio::bind_executor(self->_strand, handle_sent));// 绑定执行器

     // 发送字符
      boost::asio::async_write(
          self->_socket,// 异步写入socket
          boost::asio::buffer(text.c_str(), text.size()),// 写入字符
          boost::asio::bind_executor(self->_strand, handle_sent));// 绑定执行器
    });
  }

  // 读取数据的处理函数
  void Secondary::ReadData() {
    std::weak_ptr<Secondary> weak = shared_from_this();// 创建弱指针以避免循环引用
    boost::asio::post(_strand, [weak]() {// 在strand中异步执行
      auto self = weak.lock();// 锁定弱指针
      if (!self) return;// 如果对象已被销毁，返回
      if (self->_done) {// 如果已完成，不再处理
        return;
      }

      auto message = std::make_shared<IncomingMessage>(self->_buffer_pool->Pop());// 创建共享 IncomingMessage

     // 读取数据的处理函数
      auto handle_read_data = [weak, message](boost::system::error_code ec, size_t DEBUG_ONLY(bytes)) {
        auto self = weak.lock();// 锁定弱指针
        if (!self) return;// 如果对象已被销毁，返回
        if (!ec) {// 如果没有错误
          DEBUG_ASSERT_EQ(bytes, message->size());// 确保字节数匹配
          DEBUG_ASSERT_NE(bytes, 0u);
          // 移动缓冲区到回调函数并开始读取下一部分数据
          self->GetCommander().process_command(message->pop());
          self->ReadData();// 继续读取数据
        } else {
          // 如果发生错误，从最顶部重新开始
          log_error("secondary server: failed to read data: ", ec.message());// 记录错误
          // 连接（注释掉）
        }
      };

      // 读取头部的处理函数
      auto handle_read_header = [weak, message, handle_read_data](
        boost::system::error_code ec,
        size_t DEBUG_ONLY(bytes)) {
          auto self = weak.lock();// 锁定弱指针
          if (!self) return; // 如果对象已被销毁，返回
          if (!ec && (message->size() > 0u)) {// 如果没有错误且消息大小大于零
            DEBUG_ASSERT_EQ(bytes, sizeof(carla::streaming::detail::message_size_type));// 验证字节数
            if (self->_done) { // 如果已完成，不再处理
              return;
            }
            // 现在我们知道即将到来的缓冲区的大小，可以分配缓冲区并开始填充数据
            boost::asio::async_read(
                self->_socket,// 异步读取socket
                message->buffer(),// 读取消息缓冲区
                boost::asio::bind_executor(self->_strand, handle_read_data));// 绑定执行器
          } else if (!self->_done) {// 如果发生错误且未完成
            log_error("secondary server: failed to read header: ", ec.message());// 记录错误
            // 调试输出（注释掉）
            // 调试输出（注释掉）
            // 连接（注释掉）
          }
        };

       // 读取即将到来的缓冲区的大小.
      boost::asio::async_read(
          self->_socket,// 从socket异步读取数据
          message->size_as_buffer(),// 读取消息大小的缓冲区
          boost::asio::bind_executor(self->_strand, handle_read_header));// 绑定执行器并指定处理头部的回调函数
    });
  }

} // namespace multigpu
} // namespace carla
