// Copyright (c) YugaByte, Inc.

import Cookies from 'js-cookie';
import React from 'react';
import { Route, IndexRoute, browserHistory } from 'react-router';

import { validateToken, validateFromTokenResponse, fetchCustomerCount, customerTokenError, resetCustomer, insecureLogin } from './actions/customers';
import App from './app/App';
import Login from './pages/Login';
import Register from './pages/Register';
import AuthenticatedComponent from './pages/AuthenticatedComponent';
import Dashboard from './pages/Dashboard';
import UniverseDetail from './pages/UniverseDetail';
import Universes from './pages/Universes';
import {Tasks, TasksList, TaskDetail} from './pages/tasks';
import Alerts from './pages/Alerts';
import ListUniverse from './pages/ListUniverse';
import Metrics from './pages/Metrics';
import DataCenterConfiguration from './pages/DataCenterConfiguration';
import TableDetail from './pages/TableDetail';
import Help from './pages/Help';
import Profile from './pages/Profile';
import YugawareLogs from './pages/YugawareLogs';
import Importer from './pages/Importer';
import Certificates from './pages/Certificates';
import Releases from './pages/Releases';

const clearCredentials = () => {
  localStorage.clear();
  Cookies.remove("apiToken");
  Cookies.remove("authToken");
  Cookies.remove("customerId");
  browserHistory.push('/');
};

function validateSession(store, replacePath, callback) {
  const authToken = Cookies.get("authToken") || localStorage.getItem('authToken');
  const apiToken = Cookies.get("apiToken") || localStorage.getItem('apiToken');
  // If the token is null or invalid, we just re-direct to login page
  if((!apiToken || apiToken === '') && (!authToken || authToken === '')) {
    store.dispatch(insecureLogin()).then((response) => {
      if (response.payload.status === 200) {
        localStorage.setItem('apiToken', response.payload.data.apiToken);
        localStorage.setItem('customerId', response.payload.data.customerUUID);
      }
    });
    store.dispatch(fetchCustomerCount()).then((response) => {
      if (!response.error) {
        const responseData = response.payload.data;
        if (responseData && responseData.count === 0) {
          browserHistory.push('/register');
        }
      }
    });
    store.dispatch(customerTokenError());
    browserHistory.push('/login');
  } else {
    store.dispatch(validateToken())
      .then((response) => {
        if (response.error) {
          const { status } = response.payload;
          switch (status) {
            case 403:
              store.dispatch(resetCustomer());
              store.dispatch(customerTokenError());
              clearCredentials();
              break;
            default:
              // Do nothing
          }
          return;
        }

        store.dispatch(validateFromTokenResponse(response.payload));
        if (response.payload.status !== 200) {
          store.dispatch(resetCustomer());
          clearCredentials();
          callback();
        } else if ("uuid" in response.payload.data) {
          localStorage.setItem("customerId", response.payload.data["uuid"]);
        }
      });
  }
  callback();
}

export default (store) => {
  const authenticatedSession = (nextState, replace, callback) => {
    validateSession(store, replace, callback);
  };

  const checkIfAuthenticated = (prevState, nextState, replace, callback) => {
    validateSession(store, replace, callback);
  };

  return (
    // We will have two different routes, on which is authenticated route
    // rest un-authenticated route
    <Route path="/" component={App}>
      <Route path="/login" component={Login} />
      <Route path="/register" component={Register} />
      <Route onEnter={authenticatedSession} onChange={checkIfAuthenticated} component={AuthenticatedComponent}>
        <IndexRoute component={Dashboard} />
        <Route path="/universes" component={Universes} >
          <IndexRoute component={ListUniverse} />
          <Route path="/universes/:uuid" component={UniverseDetail} />
          <Route path="/universes/:uuid/tables/:tableUUID" component={TableDetail}/>
        </Route>
        <Route path="/tasks" component={Tasks} >
          <IndexRoute component={TasksList}/>
          <Route path="/tasks/:taskUUID" component={TaskDetail}/>
        </Route>
        <Route path="/metrics" component={Metrics} />
        <Route path="/config" component={DataCenterConfiguration}>
          <Route path="/config/:tab" component={DataCenterConfiguration} />
          <Route path="/config/:tab/:section" component={DataCenterConfiguration} />
          <Route path="/config/:tab/:section/:uuid" component={DataCenterConfiguration} />
        </Route>
        <Route path="/alerts" component={Alerts}/>
        <Route path="/help" component={Help}/>
        <Route path="/profile" component={Profile}/>
        <Route path="/logs" component={YugawareLogs}/>
        <Route path="/importer" component={Importer} />
        <Route path="/releases" component={Releases}/>
        <Route path="/certificates" component={Certificates}/>
      </Route>
    </Route>
  );
};
