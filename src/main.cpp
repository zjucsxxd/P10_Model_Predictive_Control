#include <math.h>
#include <uWS/uWS.h>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>
#include "Eigen-3.3/Eigen/Core"
#include "Eigen-3.3/Eigen/QR"
#include "MPC.h"
#include "json.hpp"

// for convenience
using json = nlohmann::json;

// For converting back and forth between radians and degrees.
constexpr double pi() { return M_PI; }
double deg2rad(double x) { return x * pi() / 180; }
double rad2deg(double x) { return x * 180 / pi(); }

// Checks if the SocketIO event has JSON data.
// If there is data the JSON object in string format will be returned,
// else the empty string "" will be returned.
string hasData(string s) {
  auto found_null = s.find("null");
  auto b1 = s.find_first_of("[");
  auto b2 = s.rfind("}]");
  if (found_null != string::npos) {
    return "";
  } else if (b1 != string::npos && b2 != string::npos) {
    return s.substr(b1, b2 - b1 + 2);
  }
  return "";
}

// Evaluate a polynomial.
double polyeval(Eigen::VectorXd coeffs, double x) {
  double result = 0.0;
  for (int i = 0; i < coeffs.size(); i++) {
    result += coeffs[i] * pow(x, i);
  }
  return result;
}

// Fit a polynomial.
// Adapted from
// https://github.com/JuliaMath/Polynomials.jl/blob/master/src/Polynomials.jl#L676-L716
Eigen::VectorXd polyfit(Eigen::VectorXd xvals, Eigen::VectorXd yvals,
                        int order) {
  assert(xvals.size() == yvals.size());
  assert(order >= 1 && order <= xvals.size() - 1);
  Eigen::MatrixXd A(xvals.size(), order + 1);

  for (int i = 0; i < xvals.size(); i++) {
    A(i, 0) = 1.0;
  }

  for (int j = 0; j < xvals.size(); j++) {
    for (int i = 0; i < order; i++) {
      A(j, i + 1) = A(j, i) * xvals(j);
    }
  }

  auto Q = A.householderQr();
  auto result = Q.solve(yvals);
  return result;
}

int main() {
  uWS::Hub h;

  // MPC is initialized here!
  MPC mpc;

  h.onMessage([&mpc](uWS::WebSocket<uWS::SERVER> ws, char *data, size_t length,
                     uWS::OpCode opCode) {
    // "42" at the start of the message means there's a websocket message event.
    // The 4 signifies a websocket message
    // The 2 signifies a websocket event
    string sdata = string(data).substr(0, length);
    cout << sdata << endl;
    if (sdata.size() > 2 && sdata[0] == '4' && sdata[1] == '2') {
      string s = hasData(sdata);
      if (s != "") {
        auto j = json::parse(s);
        string event = j[0].get<string>();
        if (event == "telemetry") {
          // j[1] is the data JSON object
          vector<double> ptsx = j[1]["ptsx"];
          vector<double> ptsy = j[1]["ptsy"];
          double px = j[1]["x"];
          double py = j[1]["y"];
          double psi = j[1]["psi"];
          double v = j[1]["speed"];
          double steering = j[1]["steering_angle"];
          double throttle = j[1]["throttle"];
          v *= 0.44704; // in m/s


          // predict vehicle state in global coordinates after latency
          double latency = 0.1;
          const double Lf = 2.67;
          double px_pred = px + v*std::cos(psi)*latency;
          double py_pred = py + v*std::sin(psi)*latency;
          double psi_pred = psi - v/Lf * latency*steering;
          double v_pred = v + throttle*9.81*latency;

          // convert ptsx and ptsy to car coordinate system for predicted px and py
          Eigen::VectorXd ptsx_cc(ptsx.size());
          Eigen::VectorXd ptsy_cc(ptsy.size());
          for (int i=0; i<ptsx.size(); i++){
              double x = ptsx[i]-px_pred;
              double y = ptsy[i]-py_pred;
              ptsx_cc[i] = x*std::cos(-psi_pred)-y*std::sin(-psi_pred);
              ptsy_cc[i] = x*std::sin(-psi_pred)+y*std::cos(-psi_pred);
          }

          // fit a 3rd order polynomial to the above ptsx_cc and ptsy_cc coordinates (in predicted car position coordinate system)
          auto coeffs = polyfit(ptsx_cc, ptsy_cc, 3);
          // calculate the cross track error
          double cte = polyeval(coeffs, 0);
          // calculate the orientation error
          double epsi = -atan(coeffs[1]);

          // define state after passed latency in predicted coordinate system
          Eigen::VectorXd state(6);
          state << 0, 0, 0, v_pred, cte, epsi;

          // solve
          auto solution = mpc.Solve(state, coeffs);

          // assign steering and throttle values
          double steer_value;
          double throttle_value;
          steer_value = -1*solution[0]/deg2rad(25);
          throttle_value = solution[1];

          std::cout <<"CTE: "<< cte<<"epsi: "<< epsi << std::endl;
          std::cout <<"Steering sent: "<< steer_value << std::endl;
          std::cout <<"Throttle sent: "<<throttle_value << std::endl;
          std::cout<<"_____________"<<std::endl;

          json msgJson;
          // NOTE: Remember to divide by deg2rad(25) before you send the steering value back.
          // Otherwise the values will be in between [-deg2rad(25), deg2rad(25] instead of [-1, 1].
          msgJson["steering_angle"] = steer_value;
          msgJson["throttle"] = throttle_value;

          //Display the MPC predicted trajectory 
          vector<double> mpc_x_vals;
          vector<double> mpc_y_vals;
          int N=(solution.size()-2)/2;
          for (int i=0; i<N; i++){
              mpc_x_vals.push_back(solution[i+2]);
              mpc_y_vals.push_back(solution[i+2+N]);
          }
          msgJson["mpc_x"] = mpc_x_vals;
          msgJson["mpc_y"] = mpc_y_vals;
          //Display the waypoints/reference line
          vector<double> next_x_vals;
          vector<double> next_y_vals;
          for (int i = 0; i<ptsx_cc.size(); i++){
              next_x_vals.push_back(ptsx_cc[i]);
              next_y_vals.push_back(ptsy_cc[i]);
          }
          msgJson["next_x"] = next_x_vals;
          msgJson["next_y"] = next_y_vals;

          auto msg = "42[\"steer\"," + msgJson.dump() + "]";
          //std::cout << msg << std::endl;
          // Latency
          // The purpose is to mimic real driving conditions where
          // the car does actuate the commands instantly.
          //
          // Feel free to play around with this value but should be to drive
          // around the track with 100ms latency.
          //
          // NOTE: REMEMBER TO SET THIS TO 100 MILLISECONDS BEFORE
          // SUBMITTING.
          this_thread::sleep_for(chrono::milliseconds(100));
          ws.send(msg.data(), msg.length(), uWS::OpCode::TEXT);
        }
      } else {
        // Manual driving
        std::string msg = "42[\"manual\",{}]";
        ws.send(msg.data(), msg.length(), uWS::OpCode::TEXT);
      }
    }
  });

  // We don't need this since we're not using HTTP but if it's removed the
  // program doesn't compile :-(
  h.onHttpRequest([](uWS::HttpResponse *res, uWS::HttpRequest req, char *data,
                     size_t, size_t) {
    const std::string s = "<h1>Hello world!</h1>";
    if (req.getUrl().valueLength == 1) {
      res->end(s.data(), s.length());
    } else {
      // i guess this should be done more gracefully?
      res->end(nullptr, 0);
    }
  });

  h.onConnection([&h](uWS::WebSocket<uWS::SERVER> ws, uWS::HttpRequest req) {
    std::cout << "Connected!!!" << std::endl;
  });

  h.onDisconnection([&h](uWS::WebSocket<uWS::SERVER> ws, int code,
                         char *message, size_t length) {
    ws.close();
    std::cout << "Disconnected" << std::endl;
  });

  int port = 4567;
  if (h.listen(port)) {
    std::cout << "Listening to port " << port << std::endl;
  } else {
    std::cerr << "Failed to listen to port" << std::endl;
    return -1;
  }
  h.run();
}
